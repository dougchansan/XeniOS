#!/usr/bin/env python3
"""Compile an explicit upstream PPC fixture set to AOT C++ (requires clang).

The checked-in assembly is the oracle. Clang is only used as a PPC assembler;
the emitted native player contains neither that assembler nor a PPC decoder.
"""
from __future__ import annotations
import argparse,importlib.util,json,pathlib,re,shutil,struct,subprocess,tempfile
SPEC=importlib.util.spec_from_file_location('exporter',pathlib.Path(__file__).parents[1]/'export.py')
m=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(m)
# Deliberately explicit coverage. New families must be added and verified, not
# silently called compatible because their generated C++ happened to compile.
FAMILIES='add addc adde addi addic addis addme addze and andc andi andis or orc ori oris xor xori xoris nand nor eqv neg subf subfc subfe subfic subfme subfze mulhd mulhdu mulhw mulhwu mulld mulli mullw divd divdu divw divwu cmp cmpi cmpl cmpli cntlzd cntlzw extsb extsh extsw rlwimi rlwinm rlwnm rldic rldicl rldicr rldimi rldcl rldcr slw sld srw srd sraw srawi srad sradi lbz lbzx lbzu lbzux lhz lhzx lhzu lhzux lha lhax lhau lhaux lwz lwzx lwzu lwzux lwa lwax ld ldx ldu ldux stb stbx stbu stbux sth sthx sthu sthux stw stwx stwu stwux std stdx stdu stdux lwbrx lhbrx stwbrx sthbrx lmw stmw lvx stvx vand vandc vor vxor vnor vsel vperm vsldoi vaddubm vadduhm vadduwm vsububm vsubuhm vsubuwm fmr fneg fabs fnabs'.split()

def elf(path):
 b=path.read_bytes()
 if b[:6]!=b'\x7fELF\x02\x02':raise ValueError('expected big-endian ELF64')
 shoff=struct.unpack_from('>Q',b,40)[0];ent,num,names=struct.unpack_from('>HHH',b,58)
 sections=[struct.unpack_from('>IIQQQQIIQQ',b,shoff+i*ent) for i in range(num)]
 def data(s):return b[s[4]:s[4]+s[5]]
 names=data(sections[names])
 def name(off):return names[off:names.index(0,off)].decode()
 ti=next(i for i,s in enumerate(sections) if name(s[0])=='.text')
 syms={}
 for s in sections:
  if s[1]!=2:continue
  strings=data(sections[s[6]])
  raw=data(s)
  for off in range(0,len(raw),s[9]):
   stn,info,other,idx,value,size=struct.unpack_from('>IBBHQQ',raw,off)
   if idx==ti and stn:syms[strings[stn:strings.index(0,stn)].decode()]=value
 return data(sections[ti]),syms

def register(reg,val,check=False):
 val=re.sub(r"(?i)([0-9a-f])(ull|llu|ll|ul|lu|u|l)$",r"\1",val.strip())
 if reg.startswith('r') and reg[1:].isdigit():expr='s.r['+reg[1:]+']';value=m.u(int(val,0))
 elif reg.startswith('f') and reg[1:].isdigit():
  expr='std::bit_cast<uint64_t>(s.f['+reg[1:]+'])';value=m.u(int(val,0) if val.startswith('0x') else struct.unpack('>Q',struct.pack('>d',float(val)))[0])
  if not check:return 's.f['+reg[1:]+']=std::bit_cast<double>(uint64_t('+value+'));'
 elif reg.startswith('v') and reg[1:].isdigit():
  words=[int(v.strip(),16) for v in val.strip('[]').split(',')]
  if len(words)!=4:raise ValueError('vector annotation')
  return ''.join((f'check(VectorWord(view,{int(reg[1:])},{i})==0x{x:x}u,"{reg}.{i}");' if check else f'VectorWord(view,{int(reg[1:])},{i},0x{x:x}u);') for i,x in enumerate(words))
 elif reg in ('cr','lr','ctr','fpscr','vrsave','xer_ca','xer_ov','xer_so'):
  expr='s.'+{'xer_ca':'ca','xer_ov':'ov','xer_so':'so'}.get(reg,reg);value=m.u(int(val,0))
 else:raise ValueError('unknown register annotation '+reg)
 return f'check({expr}=={value},"{reg}");' if check else f'{expr}={value};'

def memory(addr,val,check=False):
 a=int(addr,16);val=val.strip()
 # Match ppc_testing_main.cc: skip spaces, parse successive pairs with
 # strtoul semantics. Some upstream fixtures intentionally reflect that
 # historical parser even when the annotation contains brackets/commas.
 b=bytearray();offset=0
 while offset<len(val):
  while offset<len(val) and val[offset]==' ':offset+=1
  if offset+1>=len(val):break
  pair=val[offset:offset+2];offset+=2
  found=re.match(r'[0-9a-fA-F]+',pair)
  b.append(int(found[0],16) if found else 0)
 return ''.join((f'check(host.bytes.at(0x{a+i:x}u)=={v},"memory");' if check else f'host.bytes[0x{a+i:x}u]={v};') for i,v in enumerate(b))

def prepare(out,clang,root):
 out.mkdir(parents=True,exist_ok=False)
 fixture_dir=root/'src/xenia/cpu/ppc/testing'
 candidates=[fixture_dir/f'instr_{family}.s' for family in FAMILIES]
 report={'selected':[],'missing_fixture_files':[],'excluded':[],'tests':0,'oracle':'upstream REGISTER_OUT / MEMORY_OUT annotations','gameplay_verified':False}
 functions=[];calls=[];module_symbols=[];sources=[]
 for path in candidates:
  if not path.exists():report['missing_fixture_files'].append(path.name);continue
  src=path.read_text();asm=out/(path.stem+'.s');obj=out/(path.stem+'.o')
  asm.write_text(re.sub(r'\b(?:r|f|v)(\d+)\b',r'\1',src))
  p=subprocess.run([clang,'-target','powerpc64-unknown-linux-gnu','-mcpu=ppc64','-maltivec','-c',str(asm),'-o',str(obj)],capture_output=True,text=True)
  if p.returncode:report['excluded'].append({'fixture':path.name,'reason':'assembler: '+p.stderr[:500]});continue
  data,syms=elf(obj);base=0x82010000
  try:
   generated=out/path.stem;coverage=m.export_image(path.stem,[(base,data)],generated)
  except ValueError as error:report['excluded'].append({'fixture':path.name,'reason':str(error)});continue
  symbol=json.loads((generated/'module.json').read_text())['symbol'];module_symbols.append(symbol)
  sources.extend(sorted(generated.glob('*.cc')))
  parts=list(re.finditer(r'^(test_[A-Za-z0-9_]+):\s*$',src,re.M))
  cases=[]
  for i,part in enumerate(parts):
   text=src[part.end():parts[i+1].start() if i+1<len(parts) else len(src)];test=part[1]
   ins=[];outs=[]
   for kind,first,rest in re.findall(r'^\s*#_\s+(REGISTER_IN|REGISTER_OUT|MEMORY_IN|MEMORY_OUT)\s+(\S+)\s+([^\n]+)',text,re.M):
    value=rest.split('#')[0].strip();check=kind.endswith('OUT')
    stmt=register(first,value,check) if kind.startswith('REGISTER') else memory(first,value,check)
    (outs if check else ins).append(stmt)
   if not outs:continue
   cases.append(f'''{{ State s;auto view=s.view();const char* test="{test}";
 auto check=[&](bool ok,const char* field){{if(!ok){{std::cerr<<test<<": "<<field<<" mismatch\\n";++failures;}}}};
 // Isolate every test's data memory while retaining this fixture's code image.
 for(uint32_t a=0x10001000;a<0x10002000;++a)host.bytes[a]=0;
 {''.join(ins)}
 Runtime run(view,host,registry);auto status=run.Run(0x{base+syms[test]:08x}u,0xBCBCBCBC,100000);
 if(status!=Status::kReturned){{std::cerr<<test<<": "<<StatusName(status)<<" at "<<std::hex<<run.pc<<std::dec<<"\\n";++failures;}}
 else {{{''.join(outs)}}}++total;}}''')
  report['tests']+=len(cases);report['selected'].append({'fixture':path.name,'tests':len(cases),'instructions':coverage['instructions_total']})
  bytestr=','.join(str(x) for x in data)
  name='test_'+path.stem
  functions.append(f'''void {name}(int& failures,int& total){{TestHost host;const uint8_t code[]{{{bytestr}}};host.Map(0x82010000,code);Registry registry;std::string error;
 Require(registry.Bind({symbol}(),host,error),error.c_str());{''.join(cases)}}}''')
  calls.append(f'{name}(failures,total);')
 driver=out/'fixture_main.cc'
 driver.write_text('#include "test_host.h"\n#include <bit>\nusing namespace xe::cpu::aot;using namespace xe::cpu::aot::test;\nnamespace xe::cpu::aot {\n'+''.join(f'const Module& {s}();\n' for s in module_symbols)+'}\n'+'\n'.join(functions)+'\nint main(){int failures=0,total=0;try{'+''.join(calls)+'}catch(const std::exception&e){std::cerr<<e.what()<<"\\n";return 1;}std::cout<<total<<" PPC fixture cases; "<<failures<<" failed checks\\n";return failures?1:0;}\n')
 sources.append(driver)
 (out/'sources.cmake').write_text('set(AOT_FIXTURE_SOURCES\n'+''.join('  "'+str(p.resolve())+'"\n' for p in sources)+')\n')
 (out/'selection.json').write_text(json.dumps(report,indent=2)+'\n')
 if report['tests'] < 1000:
  raise RuntimeError(f"Too few selected fixture cases: {report['tests']}; inspect selection.json")
 print(json.dumps({'fixtures':len(report['selected']),'cases':report['tests'],'excluded':len(report['excluded']),'missing':len(report['missing_fixture_files'])}))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--clang',default='clang');args=p.parse_args()
 prepare(args.out,args.clang,pathlib.Path(__file__).resolve().parents[3])
