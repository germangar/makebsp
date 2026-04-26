/*
================
aa_filter.cl

Image-space Anti-Aliasing filter kernel.

The CPU pre-resolves all sample positions (including cross-surface seam
lookups) into a flat Sample Resolution Table (SRT). This kernel simply
gathers those pre-computed colors and averages them, which is embarrassingly
parallel across all texels.

Input layout:
  srtColors  [numValid * numSamples * 3] - pre-evaluated RGB per sample
  srtValid   [numValid * numSamples]     - 1 = valid sample, 0 = miss
  output     [totalAtlasPixels * 3]      - write-back to full atlas
  validList  [numValid]                  - GPU thread ID -> flat atlas index
  numSamples - number of samples per texel
================
*/

__kernel void aa_filter(
    __global const float *srtColors,
    __global const uchar *srtValid,
    __global       float *output,
    __global const int   *validList,
    int numSamples)
{
    int tid = get_global_id(0);
    int base = tid * numSamples;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    int   cnt = 0;

    for (int k = 0; k < numSamples; k++) {
        if (!srtValid[base + k]) continue;
        int sc = (base + k) * 3;
        r += srtColors[sc + 0];
        g += srtColors[sc + 1];
        b += srtColors[sc + 2];
        cnt++;
    }

    if (cnt > 0) {
        int idx = validList[tid] * 3;
        output[idx + 0] = r / (float)cnt;
        output[idx + 1] = g / (float)cnt;
        output[idx + 2] = b / (float)cnt;
    }
}
