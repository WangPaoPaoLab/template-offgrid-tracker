"""Commit the nRF54L15 RRAM tail before the camera target resets.

Seeed's CMSIS-DAP uploader uses load_image with buffered RRAM writes. Its
immediate reset can discard the last partial 16-byte write, corrupting static
initializers even though the upload exits successfully.
"""
Import("env")


def fix_upload_command(source, target, env):
    flags = list(env.get("UPLOADERFLAGS", []))
    for index, flag in enumerate(flags):
        if isinstance(flag, str) and "load_image {$SOURCE}; reset run" in flag:
            flags[index] = flag.replace(
                "init; mww 0x5004b500 0x101; load_image {$SOURCE}; reset run; exit",
                "init; reset halt; mww 0x5004b500 0x101; load_image {$SOURCE}; "
                "mww 0x5004b008 1; sleep 10; mww 0x5004b500 0; "
                "verify_image {$SOURCE}; reset run; exit",
            )
            if flags[index] == flag:
                raise RuntimeError("Unexpected camera upload command; RRAM commit was not inserted")
            env.Replace(UPLOADERFLAGS=flags)
            return
    raise RuntimeError("Camera uploader did not expose the expected nRF54 load_image command")


env.AddPreAction("upload", fix_upload_command)
