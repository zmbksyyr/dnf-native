"""Build the independent client launcher and verify its default key pair."""

import argparse
import json
from pathlib import Path
import subprocess

from bootstrap import ROOT, BUILD, TOOLCHAIN, external_output, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=BUILD / "login")
    args = parser.parse_args()
    work = external_output(args.build_dir)
    work.mkdir(parents=True, exist_ok=True)
    private = ROOT / "resources/login/private_key.pem"
    # Only this explicitly authorized public default pair is embedded. User
    # replacements and database credentials never enter the build inputs.
    pem = private.read_text(encoding="ascii")
    (work / "builtin_key.h").write_text(
        "#pragma once\nstatic const char BUILTIN_PRIVATE_KEY[] =\n" +
        "\n".join(json.dumps(line, ensure_ascii=True) for line in pem.splitlines(keepends=True)) +
        ";\n", encoding="ascii")
    manifest = ROOT / "src/login/dnf.manifest"
    resource = work / "login.rc"
    resource.write_text('1 24 "' + manifest.as_posix() + '"\n', encoding="ascii")
    obj = work / "login-resource.o"
    subprocess.run([str(TOOLCHAIN / "x86_64-w64-mingw32-windres.exe"),
                    "-i", str(resource), "-o", str(obj)], check=True)
    common = [str(TOOLCHAIN / "x86_64-w64-mingw32-clang.exe"), "-std=c11", "-O2",
              "-Wall", "-Wextra", "-Werror", "-static", "-Wl,--no-insert-timestamp", "-D_WIN32_WINNT=0x0601",
              "-I", str(ROOT / "src/login"), "-I", str(work)]
    crypto = str(ROOT / "src/login/crypto.c")
    test = work / "login-crypto-test.exe"
    test_command = [*common, str(ROOT / "tests/login_crypto.c"), crypto,
                    "-lcrypt32", "-ladvapi32", "-o", str(test)]
    subprocess.run(test_command, check=True)
    derived = work / "publickey.pem"
    subprocess.run([str(test), "--export-public", str(derived)], check=True)
    if derived.read_bytes() != (ROOT / "resources/login/publickey.pem").read_bytes():
        raise ValueError("Default public key differs from the actual launcher private key")
    output = work / "DNFLogin.exe"
    command = [*common, "-municode", "-mwindows", str(ROOT / "src/login/main.c"),
               str(ROOT / "src/login/mysql.c"), crypto, str(obj), "-lws2_32", "-luser32",
               "-lgdi32", "-lshell32", "-lole32", "-lcrypt32", "-ladvapi32", "-o", str(output)]
    subprocess.run(command, check=True, cwd=ROOT)
    sources = [*sorted((ROOT / "src/login").glob("*")),
               ROOT / "tests/login_crypto.c", private, ROOT / "resources/login/publickey.pem", Path(__file__)]
    receipt = {"command": command, "test_command": test_command,
               "output_sha256": sha256(output), "output_bytes": output.stat().st_size,
               "sources": {p.relative_to(ROOT).as_posix(): sha256(p) for p in sources},
               "default_public_key_sha256": sha256(derived),
               "checks": {"original_token_vectors": True, "default_key_pair": True},
               "scope": "Independent launcher build/cryptography; GUI and database checks recorded separately."}
    (work / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("Independent launcher:", output, flush=True)


if __name__ == "__main__":
    main()
