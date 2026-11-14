// Preserve the packaged script's 61-byte reset without a JS callback per item.
var dnf_native_item_reset_original = null;
function dnf_native_install_item_reset() {
    if (dnf_native_item_reset_original !== null) return;
    var target = ptr('0x080cb7d8');
    var original = target.readByteArray(22);
    var hex = Array.prototype.map.call(new Uint8Array(original), function (n) {
        return ('0' + n.toString(16)).slice(-2);
    }).join('');
    if (hex !== '5589e583ec188b4508c600008b4508c64001008b4508')
        throw new Error('Native item reset: original game instructions differ');
    // i386 SysV: zero 61 bytes, return the pointer, preserve callee-saved EDI.
    Memory.patchCode(target, 22, function (code) {
        code.writeByteArray([
            0x57, 0x8b, 0x7c, 0x24, 0x08, // push edi; mov edi,[esp+8]
            0x89, 0xfa, 0x31, 0xc0,       // mov edx,edi; xor eax,eax
            0xb9, 0x3d, 0x00, 0x00, 0x00, // mov ecx,61
            0xfc, 0xf3, 0xaa,             // cld; rep stosb
            0x89, 0xd0, 0x5f, 0xc3, 0x90  // mov eax,edx; pop edi; ret; nop
        ]);
    });
    dnf_native_item_reset_original = original;
}
function dnf_native_restore_item_reset() {
    if (dnf_native_item_reset_original === null) return;
    Memory.patchCode(ptr('0x080cb7d8'), 22, function (code) {
        code.writeByteArray(dnf_native_item_reset_original);
    });
    dnf_native_item_reset_original = null;
}
