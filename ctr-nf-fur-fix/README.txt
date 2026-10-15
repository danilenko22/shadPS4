FUR FIX - Crash Team Racing Nitro-Fueled (CUSA13795) on shadPS4
================================================================

PROBLEM
  The skin/fur of the characters (Coco, Crash, etc.) shows grey speckles.

CAUSE (found with RenderDoc)
  Fur is drawn with 6 layers (geometry shader). On layers 1-5 the pixel shader
  enters a fur "raymarching" branch (condition 0.0 < layer_index).
  On shadPS4 that branch returns an almost constant grey color -> speckles in the
  albedo G-buffer. It is not TAA nor FSR/RCAS (tested).

FIX
  The SPIR-V of the SKIN pixel shaders is patched to discard (OpKill) fur
  layers 1-5 (layer index > 0). Only the base layer is drawn:
  smooth skin, no speckles, and the base of the hair is not covered.
  (An earlier version forced the branch to false: the layers were drawn in full
  with skin color and covered the blond hair with orange. Discarded.)
  Hair (e.g. Coco's blond hair) uses the same system but with its own shaders
  (bd4b295e). Those are NOT patched: the script excludes them (EXCLUIR list).

INSTALLATION
  1. Copy shader_patch\*.spv to  %APPDATA%\shadPS4\shader\patch\
  2. Copy custom_configs\CUSA13795.json to  %APPDATA%\shadPS4\custom_configs\
     (enables patch_shaders, disables FSR and RCAS)

IF A CHARACTER STILL HAS SPECKLES
  It uses a shader with another hash that has not been patched yet:
  1. In CUSA13795.json set "dump_shaders": true
  2. Open the game and go through all characters / tracks
  3. Run:
       python -I spv_furpatch.py "%APPDATA%\shadPS4\shader\dumps" "%APPDATA%\shadPS4\shader\patch"
  4. Set "dump_shaders": false again
  5. If a character is missing hair after this, delete that character's new
     .spv from shader\patch and add its hash to EXCLUIR in the script.
