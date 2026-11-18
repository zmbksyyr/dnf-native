// Exercise real pinned ELF methods, without a database or a gameplay session.
var callbacks = [];
function check(ok, what) { if (!ok) throw new Error(what); }
function probe(level) {
    console.log('game-rules-start level=' + level);
    // Poison recycled storage: the original constructor omitted these scalar
    // initializers. Exercise its real containers as well as the new gate.
    var mailbox = Memory.alloc(0xa8);
    mailbox.writeByteArray(new Uint8Array(0xa8).fill(0xa5));
    var mailboxCtor = new NativeFunction(ptr('0x085579b2'), 'void', ['pointer'], 'sysv');
    var mailboxDtor = new NativeFunction(ptr('0x08557aac'), 'void', ['pointer'], 'sysv');
    mailboxCtor(mailbox);
    for (var offset = 0; offset < 16; offset++)
        check(mailbox.add(offset).readU8() === 0, 'mailbox initial scalar ' + offset);
    check(mailbox.add(0x7c).readU32() === 0 && mailbox.add(0x80).readU32() === 0,
          'mailbox initial letter counters');
    check(mailbox.add(0x58).readPointer().equals(mailbox.add(0x58)) &&
          mailbox.add(0x5c).readPointer().equals(mailbox.add(0x58)), 'mailbox original empty list');
    var mailboxRemain = new NativeFunction(ptr('0x08557922'), 'int', ['pointer'], 'sysv');
    check(mailboxRemain(mailbox) === 20, 'mailbox original package pool');
    mailboxDtor(mailbox);
    console.log('game-rules-mailbox-initialization');
    var dm = Memory.alloc(65536);
    var getSp = new NativeFunction(ptr('0x08360cb8'), 'int', ['pointer','int'], 'sysv');
    dm.add(8 + 4 * (0x36a2 + level)).writeU32(12345);
    dm.add(8 + 4 * (0x36a2 + level + 1)).writeU32(23456);
    check(getSp(dm,level) === 12345 && getSp(dm,level+1) === 0, 'SP lookup cap');
    console.log('game-rules-sp');
    var user = Memory.alloc(0x8ec00);
    var cap = new NativeFunction(ptr('0x0868fec8'), 'void', ['pointer','int'], 'sysv');
    cap(user,level+1);
    check(user.add(0x8ebd8).readU32() === level, 'per-user upper clamp');
    cap(user,0);
    check(user.add(0x8ebd8).readU32() === 1, 'per-user lower clamp');
    console.log('game-rules-user');
    var character = Memory.alloc(128);
    user.add(0x10).writePointer(character);
    var canUse = new NativeFunction(ptr('0x08689d06'), 'int', ['pointer','uint'], 'sysv');
    character.add(0x27).writeU16(level-1);
    check(canUse(user,0x989a13) === 1, 'level-up item before cap');
    character.add(0x27).writeU16(level);
    check(canUse(user,0x989a13) === 0, 'level-up item at cap');
    console.log('game-rules-item');
    var packet = Memory.alloc(0x17340), msg = Memory.alloc(64);
    var construct = new NativeFunction(ptr('0x0858c8c0'), 'void', ['pointer'], 'sysv');
    var setPacket = new NativeFunction(ptr('0x0858d462'), 'void', ['pointer','pointer','int'], 'sysv');
    var read = new NativeFunction(ptr('0x081db4c4'), 'int', ['pointer','pointer','pointer'], 'sysv');
    // PacketBuf starts at byte 13, after the original wire header.
    var input = Memory.alloc(15); input.add(13).writeByteArray([1,0]);
    construct(packet); setPacket(packet,input,15);
    var result = read(ptr(0),packet,msg);
    console.log('growth result=' + result + ' count=' + msg.add(0x0d).readU8() + ' selection=' + msg.add(0x0e).readU8());
    check(result === 0 && msg.add(0x0d).readU8() === 1 && msg.add(0x0e).readU8() === 0,
          'growth valid selection must parse successfully');
    construct(packet); setPacket(packet,input,14);
    check(read(ptr(0),packet,msg) === 0x4a5a, 'growth truncated selection keeps original error');
    construct(packet);
    check(read(ptr(0),packet,msg) === 0x4a55, 'growth empty packet keeps original error');
    console.log('game-rules-pass level=' + level + ' sp-cap,user-cap,item-cap,growth-valid,truncated,empty');
}
rpc.exports = {init:function () {
    var resolve = new NativeFunction(Module.getExportByName('libdp2.so','dp2_frida_resolver'),'pointer',['pointer'],'sysv');
    var main = new NativeCallback(function (state,args) {
        try { probe(parseInt(args.readUtf8String(),10)); return 42; }
        catch(e) { console.log('game-rules-failed ' + e.stack); return -1; }
    },
                                  'int',['pointer','pointer'],'sysv');
    callbacks.push(main);
    Interceptor.replace(resolve(Memory.allocUtf8String('frida.main')),main);
    Interceptor.flush();
}};
