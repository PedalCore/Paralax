# Renderer reference image

`R1-Reference.png` is a 320 x 180 RGBA8 sRGB readback of `BuildTestScene` in
`SceneRendererTests.cpp`, captured on Mesa lavapipe with validation enabled.
The camera, materials and lights are fixed in that test. It uses the renderer's
default settings, including the built-in ground material and 10% cascade blend.

The test writes the current result to `TestOutput/R1-Reference.png`. It requires
identical repeated frames, mean channel error below 2/255 and fewer than 1% of
channels differing by more than 12/255. Sparse raster-edge differences are allowed
across GPU drivers; broad lighting and color changes are not.

When deliberately changing the look, inspect the current capture and editor
screenshot, obtain an independent visual review, and then explicitly replace the
reference. Tests never update the reference automatically. Dedicated shadow
readback tests separately cover self-shadowing, internal transitions and the last
cascade fade, so the broad image tolerance cannot hide those regressions.
