// Runs with the real original game, DP2, Lua, Frida and runtime helper.
function checkItemReset() {
    var target = ptr('0x080cb7d8');
    var original = new Uint8Array(target.readByteArray(22));
    dnf_native_install_item_reset();
    dnf_native_install_item_reset();
    var reset = new NativeFunction(target, 'pointer', ['pointer'], 'sysv');
    var buffer = Memory.alloc(63);
    for (var i = 0; i < 63; ++i) buffer.add(i).writeU8(0xa5);
    check(reset(buffer.add(1)).equals(buffer.add(1)), 'reset returned pointer');
    for (var i = 1; i < 62; ++i) check(buffer.add(i).readU8() === 0, 'all reset bytes');
    check(buffer.readU8() === 0xa5 && buffer.add(62).readU8() === 0xa5, 'reset boundary');
    var getCache = new NativeFunction(ptr('0x08490f71'), 'pointer', [], 'sysv');
    var started = Date.now();
    var cache = getCache();
    check(!cache.isNull() && getCache().equals(cache), 'original 1000-entry cache singleton');
    console.log('dnf-native-cache1000-constructed milliseconds=' + (Date.now() - started));
    dnf_native_restore_item_reset();
    dnf_native_restore_item_reset();
    var restored = new Uint8Array(target.readByteArray(22));
    for (var i = 0; i < 22; ++i) check(restored[i] === original[i], 'reset restore');
    console.log('dnf-native-reset61-boundaries-and-restore-ok');
}
