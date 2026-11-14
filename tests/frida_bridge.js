// The driver pins the original ELF. This fixture never replaces the DP2/Frida libraries.
var callbacks = [];
var luaState = null;
var calls = 0;
var resolver = null;
var callLua = null;

function check(value, message) {
    if (!value) throw new Error('bridge fixture: ' + message);
}

function resolve(name) {
    return resolver(Memory.allocUtf8String(name));
}

function codeBytes(address) {
    return Array.prototype.map.call(new Uint8Array(address.readByteArray(14)), function (n) {
        return ('0' + n.toString(16)).slice(-2);
    }).join('');
}

function checkGameHook() {
    var getEnvironment = new NativeFunction(ptr('0x080cc181'), 'pointer', [], 'sysv');
    var address = ptr('0x08106ce0');
    var original = '5589e58b45088b80780300005dc3';
    check(codeBytes(address) === original, 'original game instructions');
    var environment = getEnvironment();
    check(!environment.isNull(), 'game environment pointer');
    var getGroup = new NativeFunction(address, 'int', ['pointer'], 'sysv');
    // The DP2 entry script runs before game configuration finishes loading.
    var baseline = getGroup(environment);
    var changed = baseline ^ 64;
    var enters = 0, leaves = 0;
    var hook = Interceptor.attach(address, {
        onEnter: function (args) {
            check(args[0].equals(environment), 'game hook this pointer');
            ++enters;
        },
        onLeave: function (retval) {
            check(retval.toInt32() === baseline, 'original game result before hook change');
            ++leaves;
            retval.replace(changed);
        }
    });
    Interceptor.flush();
    check(getGroup(environment) === changed && enters === 1 && leaves === 1, 'game hook execution');
    hook.detach();
    Interceptor.flush();
    check(getGroup(environment) === baseline && enters === 1 && leaves === 1, 'game hook removal');
    check(codeBytes(address) === original, 'restored game instructions');
    console.log('dnf-native-original-game-hook-ok original=' + baseline + ' hooked=' + changed + ' restored=' + baseline);
}

rpc.exports = {
    init: function () {
        resolver = new NativeFunction(Module.getExportByName('libdp2.so', 'dp2_frida_resolver'),
                                      'pointer', ['pointer'], 'sysv');
        callLua = new NativeFunction(resolve('lua.call'), 'int', ['int', 'float', 'pointer'], 'sysv');
        var main = new NativeCallback(function (state, args) {
            check(!state.isNull() && args.readUtf8String() === 'bridge-main', 'main arguments');
            luaState = state;
            checkGameHook();
            return 1401;
        }, 'int', ['pointer', 'pointer'], 'sysv');
        var handler = new NativeCallback(function (state, i, f, s) {
            check(state.equals(luaState), 'Lua state identity');
            check(i >= 1 && i <= 32 && f === i + 0.25, 'Lua/JS integer and float');
            check(s.readUtf8String() === 'lua-to-js-' + i, 'Lua/JS string');
            var result = callLua(1000 + i, i + 0.5, Memory.allocUtf8String('js-to-lua-' + i));
            check(result === (1000 + i) * 3, 'nested JS/Lua return');
            ++calls;
            if (calls === 32) console.log('dnf-native-js-lua-roundtrip-ok calls=32');
            return i * 7;
        }, 'int', ['pointer', 'int', 'float', 'pointer'], 'sysv');
        callbacks.push(main, handler);
        Interceptor.replace(resolve('frida.main'), main);
        Interceptor.replace(resolve('frida.handler'), handler);
        Interceptor.flush();
    }
};
