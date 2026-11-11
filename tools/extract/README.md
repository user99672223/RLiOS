# Asset extraction

1. `upk_unpack.py <keyfile> <out> <pkg>...` decrypts + decompresses RL packages (format notes in the script).
   Key file lives in `work/keys/` (captured by `tools/hook`, never committed).
2. UE Viewer (umodel) built from source in `work/refs/UEViewer` with `umodel_rl34.patch` applied
   (accept package licensee version up to 40; read our pre-unpacked packages without its own RL decryption path,
   whose chunk table layout is out of date). Link the game's `*.tfc` texture caches next to the unpacked packages.
       umodel -game=rocketleague -path=work/upk_dec -export -gltf -png -out=work/assets_raw <Package>
