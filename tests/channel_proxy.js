// test_bridge.py pins the original game ELF. Only the fixture's environment
// port is set temporarily; the original connect, version and download code runs.
var callbacks = [];

rpc.exports = {
    init: function () {
        var resolver = new NativeFunction(Module.getExportByName('libdp2.so', 'dp2_frida_resolver'),
                                          'pointer', ['pointer'], 'sysv');
        var main = new NativeCallback(function () {
            var getenv = new NativeFunction(Module.getExportByName('libc.so.6', 'getenv'),
                                            'pointer', ['pointer'], 'sysv');
            var port = parseInt(getenv(Memory.allocUtf8String('DNF_NATIVE_BRIDGE_PORT')).readUtf8String(), 10);
            var getEnvironment = new NativeFunction(ptr('0x080cc181'), 'pointer', [], 'sysv');
            var stringCtor = new NativeFunction(ptr('0x08707630'), 'void', ['pointer', 'pointer', 'pointer'], 'sysv');
            var proxyCtor = new NativeFunction(ptr('0x081060f4'), 'void', ['pointer', 'pointer', 'int'], 'sysv');
            var initProxy = new NativeFunction(ptr('0x0810614a'), 'bool', ['pointer'], 'sysv');
            var checkScript = new NativeFunction(ptr('0x081066ea'), 'bool', ['pointer'], 'sysv');
            var environment = getEnvironment();
            var saved = environment.add(0x250).readU32();
            environment.add(0x250).writeU32(port);
            try {
                var address = Memory.alloc(4), allocator = Memory.alloc(4), proxy = Memory.alloc(0x80020);
                stringCtor(address, Memory.allocUtf8String('127.0.0.1'), allocator);
                proxyCtor(proxy, address, port);
                if (!initProxy(proxy)) throw new Error('Original channel proxy initialization failed');
                return checkScript(proxy) ? 42 : 43;
            } finally {
                environment.add(0x250).writeU32(saved);
            }
        }, 'int', ['pointer', 'pointer'], 'sysv');
        callbacks.push(main);
        Interceptor.replace(resolver(Memory.allocUtf8String('frida.main')), main);
        Interceptor.flush();
    }
};
