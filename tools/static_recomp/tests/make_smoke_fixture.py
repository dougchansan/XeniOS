#!/usr/bin/env python3
"""Self-authored PPC program for real XeniOS CPU/HLE/memory integration tests."""
import importlib.util,json,pathlib,struct,sys
p=pathlib.Path(__file__).parents[1]/'export.py';s=importlib.util.spec_from_file_location('exporter',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
# mflr r30; li r3,7; li r4,6; add r3,r3,r4; stw r3,0(r5);
# bl import; mtctr r6; bctrl; stw r3,4(r5); mtlr r30; blr.
words=[0x7fc802a6,0x38600007,0x38800006,0x7c632214,0x90650000,
       0x4800002d,0x7cc903a6,0x4e800421,0x90650004,0x7fc803a6,0x4e800020]
words += [0x60000000]*(16-len(words))
words += [0x44000042,0x4e800020,0x60000000,0x60000000]
data=struct.pack('>'+'I'*len(words),*words)
generated=out/'generated';m.export_image('synthetic-smoke',[(0x82010000,data)],generated)
info=json.loads((generated/'module.json').read_text());symbol=info['symbol']
(out/'fixture.h').write_text('#pragma once\n#include "xenia/cpu/backend/static/runtime.h"\nnamespace xe::cpu::aot { const Module& '+symbol+'(); }\ninline const auto& SmokeModule(){return xe::cpu::aot::'+symbol+'();}\ninline constexpr uint8_t smoke_code[]={'+','.join(str(v) for v in data)+'};\n')
(out/'sources.cmake').write_text('set(SMOKE_SOURCES\n'+''.join('"'+str(f.resolve())+'"\n' for f in sorted(generated.glob('*.cc')))+')\n')
