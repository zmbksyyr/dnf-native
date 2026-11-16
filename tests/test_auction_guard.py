"""Run the production DP2 packet guard in the packaged original Gadget engine."""
from pathlib import Path
import json
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from bootstrap import BUILD
ROOT=Path(__file__).resolve().parents[1]

def main():
    work=BUILD/"auction-guard";work.mkdir(exist_ok=True)
    source=(ROOT/"src/controller/auction_maintenance.h").read_text(encoding="utf-8")
    block=source.split('return R"JS(',1)[1].split(')JS";',1)[0]
    # Test doubles cover only external callbacks and SQL responses. The exact
    # production guard operates on real native memory inside the original JS VM.
    script=r'''
var retained = [];
function testGuard() {
    var hook, queries=0, calls=0, rows=0, hex=null, group=1, missing=false, success=1, nested=false;
    var packet=Memory.alloc(200); packet.writeByteArray(new Array(200).fill(0)); packet.add(5).writeU8(1);
    packet.add(54).writeU32(16400); packet.add(76).writeU32(123);
    function NativeFunction() { return function () { ++calls; return 777; }; }
    function NativeCallback(fn) { return fn; }
    var Interceptor={replace:function(address,fn) {hook=fn;}};
    function G_CDataManager() {return ptr(1);}
    function CDataManager_find_item() {return missing?ptr(0):ptr(1);}
    function CItem_getItemGroupName() {return group;}
    var mysql_frida=ptr(1);
    function api_MySQL_exec(db,sql) {
        ++queries;
        if(sql.indexOf('equ_id=123;')<0) throw new Error('wrong SQL identity');
        if(nested) {nested=false;if(hook(ptr(1),ptr(2),packet,143)!==777)throw new Error('nested result');}
        return success;
    }
    function MySQL_get_n_rows() {return rows;}
    function MySQL_fetch() {return 1;}
    function api_MySQL_get_str() {return hex;}
    PRODUCTION_BLOCK
    function check(value,message) {if(!value)throw new Error('auction guard: '+message);}
    function seed() {packet.add(106).writeByteArray(new Array(30).fill(85));}
    function unchanged() {return Array.prototype.every.call(new Uint8Array(packet.add(106).readByteArray(30)),function(x){return x===85;});}
    seed();check(hook(ptr(1),ptr(2),packet,143)===777 && unchanged(),'ordinary equipment without SQL record');
    rows=1;hex='01';seed();hook(ptr(1),ptr(2),packet,143);check(unchanged(),'short record');
    hex=new Array(31).join('00');seed();hook(ptr(1),ptr(2),packet,143);check(unchanged(),'empty sockets');
    hex='01'+new Array(30).join('ab');seed();hook(ptr(1),ptr(2),packet,143);
    check(packet.add(106).readU8()===1 && packet.add(135).readU8()===171,'valid record applied exactly30 bytes');
    packet.add(136).writeU8(67);seed();nested=true;var q=queries;hook(ptr(1),ptr(2),packet,143);check(queries===q+1 && packet.add(136).readU8()===67,'reentry and boundary');
    packet.add(76).writeU32(1);q=queries;seed();hook(ptr(1),ptr(2),packet,143);check(queries===q && unchanged(),'sentinel instance no SQL');packet.add(76).writeU32(123);
    packet.add(54).writeU32(0);q=queries;seed();hook(ptr(1),ptr(2),packet,143);check(queries===q && unchanged(),'zero item no SQL');packet.add(54).writeU32(16400);
    missing=true;q=queries;seed();hook(ptr(1),ptr(2),packet,143);check(queries===q && unchanged(),'missing metadata');missing=false;
    group=0;q=queries;seed();hook(ptr(1),ptr(2),packet,143);check(queries===q && unchanged(),'stackable retained');group=1;
    success=0;seed();hook(ptr(1),ptr(2),packet,143);check(unchanged(),'SQL failure retained');
    console.log('DNF_NATIVE_AUCTION_GUARD_PASS calls='+calls+' queries='+queries);
}
rpc.exports={init:function() {
    var resolver=new NativeFunction(Module.getExportByName('libdp2.so','dp2_frida_resolver'),'pointer',['pointer'],'sysv');
    var main=new NativeCallback(function(){testGuard();return 42;},'int',['pointer','pointer'],'sysv');
    retained.push(main);Interceptor.replace(resolver(Memory.allocUtf8String('frida.main')),main);Interceptor.flush();
}};
'''.replace("PRODUCTION_BLOCK",block)
    js=work/"guard.js";js.write_text(script,encoding="utf-8")
    lua=work/"guard.lua";lua.write_text('local frida=require("df.frida")\nassert(frida.load("auction-guard")==42)\nprint("auction guard engine done")\nos.exit(42)\n',encoding="utf-8")
    subprocess.run([sys.executable,"-B",str(ROOT/"tests/game_fixture.py"),"--lua-script",str(lua),"--frida-script",str(js)],check=True,cwd=ROOT)
    receipt=json.loads((BUILD/"game-probe.json").read_text());root=Path(receipt["command"][2]);stdout=(root/"stdout.log").read_text(encoding="utf-8",errors="replace")
    assert "DNF_NATIVE_AUCTION_GUARD_PASS" in stdout
    receipt["scope"]="Exact production guard in original Gadget VM, real packet memory and deterministic SQL/metadata callbacks; no client or SQL business acceptance"
    receipt["passed"]=True;(work/"receipt.json").write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
    print("Auction DP2 guard PASS",work/"receipt.json")

if __name__=="__main__":main()
