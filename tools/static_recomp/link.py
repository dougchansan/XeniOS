#!/usr/bin/env python3
"""Create an immutable private source catalog for compile-time module linking."""
from __future__ import annotations
import argparse,hashlib,json,pathlib,re,shutil,sys,tempfile

def link(modules:list[pathlib.Path],out:pathlib.Path):
 if out.exists():raise ValueError('catalog destination already exists')
 validated=[];names=set();symbols=set()
 for root in modules:
  info=json.loads((root/'module.json').read_text())
  if info.get('format')!='xenios-aot-link-v1' or info.get('abi')!=1:raise ValueError('module ABI mismatch')
  symbol=info.get('symbol','');name=info.get('name','')
  if not re.fullmatch(r'aot_[0-9a-f]{16}_module',symbol):raise ValueError('invalid entry symbol')
  if not name or name in names or symbol in symbols:raise ValueError('duplicate or missing module identity')
  names.add(name);symbols.add(symbol)
  sources=info.get('sources')
  if not isinstance(sources,dict) or not sources:raise ValueError('source integrity manifest is required; regenerate this module')
  files=[]
  for filename,sha in sources.items():
   if not re.fullmatch(r'aot_[0-9a-f]{16}_(?:module|[0-9]+)\.cc',filename):raise ValueError('invalid source filename')
   path=root/filename
   if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=sha:raise ValueError('generated source integrity mismatch')
   files.append(path)
  if set(p.name for p in root.glob('*.cc'))!=set(sources):raise ValueError('unexpected generated sources')
  validated.append((info,files))
 if not validated:raise ValueError('at least one module is required')
 out.parent.mkdir(parents=True,exist_ok=True);tmp=pathlib.Path(tempfile.mkdtemp(prefix='.catalog-',dir=out.parent))
 try:
  entries=[];cpp=['#include "xenia/cpu/backend/static/runtime.h"','namespace xe::cpu::aot {']
  cpp += [f'const Module& {info["symbol"]}();' for info,_ in validated]
  cpp += ['}','namespace xe::cpu::backend::statik {','std::span<const aot::Module* const> LinkedModules() {','static const aot::Module* const modules[] = {']
  cpp += [f'&aot::{info["symbol"]}(),' for info,_ in validated]
  cpp += ['}; return modules;','}','}']
  (tmp/'catalog.cc').write_text('\n'.join(cpp)+'\n')
  for i,(info,files) in enumerate(validated):
   folder=tmp/f'module_{i:03d}';folder.mkdir()
   for src in files:
    shutil.copyfile(src,folder/src.name);entries.append(f'module_{i:03d}/{src.name}')
  entries.append('catalog.cc')
  (tmp/'catalog.cmake').write_text('set(XENIA_AOT_SOURCES\n'+''.join('  "${CMAKE_CURRENT_LIST_DIR}/'+f+'"\n' for f in entries)+')\n')
  (tmp/'catalog.json').write_text(json.dumps({'abi':1,'modules':[info for info,_ in validated],'cpu_jit':False,'gameplay_verified':False},indent=2)+'\n')
  (tmp/'.gitignore').write_text('*\n')
  tmp.rename(out)
 except BaseException:shutil.rmtree(tmp,ignore_errors=True);raise
 return len(validated)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('modules',type=pathlib.Path,nargs='+');p.add_argument('--out',type=pathlib.Path,required=True);a=p.parse_args()
 try:print(f'Linked {link(a.modules,a.out)} module source packages. Compile and sign the application before execution.')
 except (ValueError,OSError,KeyError,TypeError) as e:print(f'link failed: {e}',file=sys.stderr);return 1
 return 0
if __name__=='__main__':raise SystemExit(main())
