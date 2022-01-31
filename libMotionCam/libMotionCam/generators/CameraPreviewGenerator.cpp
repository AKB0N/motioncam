#include <Halide.h>

#include <vector>
#include <functional>
#include <limits>

#include "Common.h"

using namespace Halide;
using namespace Halide::ConciseCasts;

using std::vector;
using std::function;
using std::pair;

const Expr HALF_YUV_R = cast<float16_t>(0.299f);
const Expr HALF_YUV_G = cast<float16_t>(0.587f);
const Expr HALF_YUV_B = cast<float16_t>(0.114f);

class CameraVideoPreviewGenerator : public Halide::Generator<CameraVideoPreviewGenerator> {
public:
    GeneratorParam<int> downscale_factor{"downscale_factor", 2};
    GeneratorParam<int> pixel_format{"pixel_format", 0};

    Input<Buffer<uint8_t>> input{"input", 1};
    Input<int> stride{"stride"};
    Input<float[3]> asShotVector{"asShotVector"};
    Input<float[3]> wbOffset{"wbOffset"};

    Input<Buffer<float>> cameraToSrgb{"cameraToSrgb", 2};

    Input<int> width{"width"};
    Input<int> height{"height"};

    Input<float[4]> blackLevel{"blackLevel"};
    Input<float> whiteLevel{"whiteLevel"};

    Input<Buffer<float>[4]> shadingMap{"shadingMap", 2 };
    Input<int> sensorArrangement{"sensorArrangement"};

    Input<float> gamma{"gamma"};

    Output<Buffer<uint8_t>> output{"output", 3};

protected:
    Var v_i{"i"};
    Var v_x{"x"};
    Var v_y{"y"};
    Var v_c{"c"};
    
    Var v_xo{"xo"};
    Var v_xi{"xi"};
    Var v_yo{"yo"};
    Var v_yi{"yi"};

    Var v_xio{"xio"};
    Var v_xii{"xii"};
    Var v_yio{"yio"};
    Var v_yii{"yii"};

    Var subtile_idx{"subtile_idx"};
    Var tile_idx{"tile_idx"};

public:
    void generate();
    Func downscale(Func in, Func& temp);
    void transform(Func& output, Func input, Func m);

    void linearScale16(Func& result, Func image, Expr fromWidth, Expr fromHeight, Expr toWidth, Expr toHeight);
};

Func CameraVideoPreviewGenerator::downscale(Func f, Func& downx) {
    Func in{"downscaleIn"}, downy{"downy"}, result{"downscaled"};

    const int downscale = downscale_factor;
    RDom r(-downscale/2, downscale+1);

    in(v_x, v_y, v_c) = cast<float16_t>(f(v_x, v_y, v_c));

    downx(v_x, v_y, v_c) = sum(in(v_x * downscale + r.x, v_y, v_c)) / cast<float16_t>(downscale + 1);
    downy(v_x, v_y, v_c) = sum(downx(v_x, v_y * downscale + r.x, v_c)) / cast<float16_t>(downscale + 1);
    
    result(v_x, v_y, v_c) = cast<float16_t>(downy(v_x, v_y, v_c));

    return result;
}

void CameraVideoPreviewGenerator::linearScale16(Func& result, Func image, Expr fromWidth, Expr fromHeight, Expr toWidth, Expr toHeight) {
    Expr scaleX = toWidth * cast<float16_t>(1.0f) / (cast<float16_t>(fromWidth));
    Expr scaleY = toHeight * cast<float16_t>(1.0f) / (cast<float16_t>(fromHeight));
    
    Expr HALF_0_5 = cast<float16_t>(0.5f);

    Expr fx = max(0, (v_x + HALF_0_5) * (cast<float16_t>(1.0f) / scaleX) - HALF_0_5);
    Expr fy = max(0, (v_y + HALF_0_5) * (cast<float16_t>(1.0f) / scaleY) - HALF_0_5);
    
    Expr x = cast<int>(fx);
    Expr y = cast<int>(fy);
    
    Expr a = fx - x;
    Expr b = fy - y;

    Expr x0 = clamp(x, 0, fromWidth - 1);
    Expr y0 = clamp(y, 0, fromHeight - 1);

    Expr x1 = clamp(x + 1, 0, fromWidth - 1);
    Expr y1 = clamp(y + 1, 0, fromHeight - 1);
    
    Expr p0 = lerp(cast<float16_t>(image(x0, y0)), cast<float16_t>(image(x1, y0)), a);
    Expr p1 = lerp(cast<float16_t>(image(x0, y1)), cast<float16_t>(image(x1, y1)), a);

    result(v_x, v_y) = cast<float16_t>(lerp(p0, p1, b));
}

void CameraVideoPreviewGenerator::transform(Func& output, Func input, Func m) {
    Expr ir = input(v_x, v_y, 0);
    Expr ig = input(v_x, v_y, 1);
    Expr ib = input(v_x, v_y, 2);

    // Color correct
    Expr r = cast<float16_t>(m(0, 0)) * ir + cast<float16_t>(m(1, 0)) * ig + cast<float16_t>(m(2, 0)) * ib;
    Expr g = cast<float16_t>(m(0, 1)) * ir + cast<float16_t>(m(1, 1)) * ig + cast<float16_t>(m(2, 1)) * ib;
    Expr b = cast<float16_t>(m(0, 2)) * ir + cast<float16_t>(m(1, 2)) * ig + cast<float16_t>(m(2, 2)) * ib;
    
    output(v_x, v_y, v_c) = select(v_c == 0, clamp(r, cast<float16_t>(0.0f), cast<float16_t>(1.0f)),
                                   v_c == 1, clamp(g, cast<float16_t>(0.0f), cast<float16_t>(1.0f)),
                                             clamp(b, cast<float16_t>(0.0f), cast<float16_t>(1.0f)));
}

void CameraVideoPreviewGenerator::generate() {
    Func bayer{"bayer"};
    Func linear{"linear"};
    Func colorCorrected{"colorCorrected"};
    Func gammaContrastLut{"gammaContrastLut"};
    Func inputRepeated{"inputRepeated"};
    Func scaledShadingMap[4] { Func("scaledShadingMap[0]"), Func("scaledShadingMap[1]"), Func("scaledShadingMap[2]"), Func("scaledShadingMap[3]") };
    Func shadingMapInput{"shadingMapInput"};
    Func rawInput{"rawInput"};
    Func downscaled{"downscaled"};
    Func downscaledTemp{"downscaledTemp"};
    Func demosaicInput("demosaicInput");
    Func blackLevelFunc{"blackLevelFunc"};
    Func linearFunc{"linearFunc"};
    Func clampedInput{"clampedInput"};
    Func asShotFunc{"asShotFunc"};
    Func wbOffsetFunc{"wbOffsetFunc"};
    Func demosaiced{"demosaiced"};

    Expr HALF_0_0 = cast<float16_t>(0.0f);
    Expr HALF_0_5 = cast<float16_t>(0.5f);
    Expr HALF_1_0 = cast<float16_t>(1.0f);

    inputRepeated(v_i) = cast<uint16_t>(input(v_i));

    for(int c = 0; c < 4; c++) {
        linearScale16(scaledShadingMap[c], shadingMap[c], shadingMap[c].width(), shadingMap[c].height(), width, height);
    }

    if(pixel_format == static_cast<int>(RawFormat::RAW10)) {
        Expr X = (v_x / 4) * 4;
        Expr xoffset = (v_y * stride) + 10*X/8;
        
        Expr p = v_x - X;
        Expr shift = p * 2;

        bayer(v_x, v_y) = (inputRepeated(xoffset + p) << 2) | ((inputRepeated(xoffset + 4) >> shift) & 0x03);
    }
    else if(pixel_format == static_cast<int>(RawFormat::RAW12)) {
        Expr X = (v_x / 2) * 2;
        Expr xoffset = (v_y*stride) + 12*X/8;

        Expr p = v_x - X;
        Expr shift = p * 4;

        bayer(v_x, v_y) = (inputRepeated(xoffset + p)) << 4 | ((inputRepeated(xoffset + 2) >> shift) & 0x0F);

    }    
    else if(pixel_format == static_cast<int>(RawFormat::RAW16)) {
        Expr offset = (v_y*stride) + (v_x*2);

        bayer(v_x, v_y) = inputRepeated(offset) | (inputRepeated(offset + 1) << 8);
    }
    else {
        throw std::runtime_error("invalid pixel format");
    }

    rawInput(v_x, v_y, v_c) = select(
        v_c == 0, bayer(v_x*2,      v_y*2),
        v_c == 1, bayer(v_x*2 + 1,  v_y*2),
        v_c == 2, bayer(v_x*2,      v_y*2 + 1),
                  bayer(v_x*2 + 1,  v_y*2 + 1));

    clampedInput(v_x, v_y, v_c) = rawInput(clamp(v_x, 0, width*downscale_factor-1), clamp(v_y, 0, height*downscale_factor-1), v_c);

    demosaicInput(v_x, v_y, v_c) =
        select(sensorArrangement == static_cast<int>(SensorArrangement::RGGB),
                select( v_c == 0, clampedInput(v_x, v_y, 0),
                        v_c == 1, clampedInput(v_x, v_y, 1),
                                  clampedInput(v_x, v_y, 3) ),

            sensorArrangement == static_cast<int>(SensorArrangement::GRBG),
                select( v_c == 0, clampedInput(v_x, v_y, 1),
                        v_c == 1, clampedInput(v_x, v_y, 0),
                                  clampedInput(v_x, v_y, 2) ),

            sensorArrangement == static_cast<int>(SensorArrangement::GBRG),
                select( v_c == 0, clampedInput(v_x, v_y, 2),
                        v_c == 1, clampedInput(v_x, v_y, 0),
                                  clampedInput(v_x, v_y, 1) ),

                select( v_c == 0, clampedInput(v_x, v_y, 3),
                        v_c == 1, clampedInput(v_x, v_y, 1),
                                  clampedInput(v_x, v_y, 0) ) );

    shadingMapInput(v_x, v_y, v_c) =
        mux(v_c, { scaledShadingMap[0](v_x, v_y), scaledShadingMap[1](v_x, v_y), scaledShadingMap[3](v_x, v_y) } );

    downscaled = downscale(demosaicInput, downscaledTemp);

    blackLevelFunc(v_c) = cast<float16_t>(
                            select( v_c == 0, blackLevel[0],
                                    v_c == 1, blackLevel[1],
                                              blackLevel[3]) );

    linearFunc(v_c) = cast<float16_t>(
                      select(   v_c == 0, 1.0f / (whiteLevel - blackLevelFunc(v_c)),
                                v_c == 1, 1.0f / (whiteLevel - blackLevelFunc(v_c)),
                                          1.0f / (whiteLevel - blackLevelFunc(v_c))) );

    blackLevelFunc.compute_root().unroll(v_c);
    linearFunc.compute_root().unroll(v_c);

    linear(v_x, v_y, v_c) = cast<float16_t>(
        (downscaled(v_x, v_y, v_c) - blackLevelFunc(v_c)) * linearFunc(v_c));

    asShotFunc(v_c) = mux(v_c, { asShotVector[0], asShotVector[1], asShotVector[2] } );
    wbOffsetFunc(v_c) = mux(v_c, { wbOffset[0], wbOffset[1], wbOffset[2] } );

    demosaiced(v_x, v_y, v_c) = cast<float16_t>(wbOffsetFunc(v_c)) * shadingMapInput(v_x, v_y, v_c) * clamp(linear(v_x, v_y, v_c), cast<float16_t>(0.0f), cast<float16_t>(asShotFunc(v_c)));

    transform(colorCorrected, demosaiced, cameraToSrgb);

    // Gamma correct
    Expr g = pow(colorCorrected(v_x, v_y, v_c), HALF_1_0 / cast<float16_t>(gamma));

    output(v_x, v_y, v_c) =
        select(v_c < 3,
            cast<uint8_t>(clamp(g * cast<float16_t>(255) + HALF_0_5, cast<float16_t>(0), cast<float16_t>(255))),
            255);

    // Output interleaved
    output
        .dim(0).set_stride(4)
        .dim(2).set_stride(1);

    if (get_target().has_gpu_feature()) {
        int tx = 8;
        int ty = 4;

        if(downscale_factor == 4) {
            tx = ty = 4;
        }

        clampedInput
            .reorder(v_c, v_x, v_y)
            .compute_at(downscaled, v_x)            
            .gpu_threads(v_x, v_y);

        downscaledTemp
            .reorder(v_c, v_x, v_y)
            .compute_at(downscaled, v_x)
            .vectorize(v_c)
            .gpu_threads(v_x, v_y);            

        downscaled
            .compute_root()
            .reorder(v_c, v_x, v_y)
            .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);
     
        output
            .bound(v_c, 0, 4)
            .compute_root()
            .reorder(v_c, v_x, v_y)
            .unroll(v_c)
            .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);
    }
    else {
        // TODO: Better schedule
        clampedInput.compute_root();
        downscaledTemp.compute_root();
        downscaled.compute_root();
        output.compute_root();
    }    
}

//

class CameraPreviewGenerator : public Halide::Generator<CameraPreviewGenerator> {
public:
    GeneratorParam<int> tonemap_levels{"tonemap_levels", 7};
    GeneratorParam<int> downscale_factor{"downscale_factor", 2};
    GeneratorParam<int> pixel_format{"pixel_format", 0};

    Input<Buffer<uint8_t>> input{"input", 1};
    Input<int> stride{"stride"};
    Input<float[3]> asShotVector{"asShotVector"};
    Input<float[3]> wbOffset{"wbOffset"};
    Input<Buffer<float>> cameraToSrgb{"cameraToSrgb", 2};
    Input<bool> flipped{"flipped", false};

    Input<int> width{"width"};
    Input<int> height{"height"};

    Input<float[4]> blackLevel{"blackLevel"};
    Input<float> whiteLevel{"whiteLevel"};

    Input<Buffer<float>[4]> shadingMap{"shadingMap", 2 };
    Input<int> sensorArrangement{"sensorArrangement"};

    Input<float> tonemapVariance{"tonemapVariance"};
    Input<float> gamma{"gamma"};
    Input<float> shadows{"shadows"};
    Input<float> blacks{"blacks"};
    Input<float> whitePoint{"whitePoint"};
    Input<float> contrast{"contrast"};
    Input<float> saturation{"saturation"};

    Output<Buffer<uint8_t>> output{"output", 3};

protected:
    Var v_i{"i"};
    Var v_x{"x"};
    Var v_y{"y"};
    Var v_c{"c"};
    
    Var v_xo{"xo"};
    Var v_xi{"xi"};
    Var v_yo{"yo"};
    Var v_yi{"yi"};

    Var v_xio{"xio"};
    Var v_xii{"xii"};
    Var v_yio{"yio"};
    Var v_yii{"yii"};

    Var subtile_idx{"subtile_idx"};
    Var tile_idx{"tile_idx"};

public:
    void generate();
    Func downscale(Func in, Func& temp);
    Func tonemap(Func input, Expr gain, Expr gamma, Expr variance);
    void transform(Func& output, Func input, Func m);

    vector<pair<Func, Func>> buildPyramid(Func input, int maxlevel);
    void pyramidDown(Func& output, Func& intermediate, Func input);
    void pyramidUp(Func& output, Func& intermediate, Func input);
    void linearScale16(Func& result, Func image, Expr fromWidth, Expr fromHeight, Expr toWidth, Expr toHeight);
};

void CameraPreviewGenerator::linearScale16(Func& result, Func image, Expr fromWidth, Expr fromHeight, Expr toWidth, Expr toHeight) {
    Expr scaleX = toWidth * cast<float16_t>(1.0f) / (cast<float16_t>(fromWidth));
    Expr scaleY = toHeight * cast<float16_t>(1.0f) / (cast<float16_t>(fromHeight));
    
    Expr HALF_0_5 = cast<float16_t>(0.5f);

    Expr fx = max(0, (v_x + HALF_0_5) * (cast<float16_t>(1.0f) / scaleX) - HALF_0_5);
    Expr fy = max(0, (v_y + HALF_0_5) * (cast<float16_t>(1.0f) / scaleY) - HALF_0_5);
    
    Expr x = cast<int>(fx);
    Expr y = cast<int>(fy);
    
    Expr a = fx - x;
    Expr b = fy - y;

    Expr x0 = clamp(x, 0, fromWidth - 1);
    Expr y0 = clamp(y, 0, fromHeight - 1);

    Expr x1 = clamp(x + 1, 0, fromWidth - 1);
    Expr y1 = clamp(y + 1, 0, fromHeight - 1);
    
    Expr p0 = lerp(cast<float16_t>(image(x0, y0)), cast<float16_t>(image(x1, y0)), a);
    Expr p1 = lerp(cast<float16_t>(image(x0, y1)), cast<float16_t>(image(x1, y1)), a);

    result(v_x, v_y) = cast<float16_t>(lerp(p0, p1, b));
}

Func CameraPreviewGenerator::downscale(Func f, Func& downx) {
    Func in{"downscaleIn"}, downy{"downy"}, result{"downscaled"};

    const int downscale = downscale_factor;
    RDom r(-downscale/2, downscale+1);

    in(v_x, v_y, v_c) = cast<float16_t>(f(v_x, v_y, v_c));

    downx(v_x, v_y, v_c) = sum(in(v_x * downscale + r.x, v_y, v_c)) / cast<float16_t>(downscale + 1);
    downy(v_x, v_y, v_c) = sum(downx(v_x, v_y * downscale + r.x, v_c)) / cast<float16_t>(downscale + 1);
    
    result(v_x, v_y, v_c) = cast<float16_t>(downy(v_x, v_y, v_c));

    return result;
}

void CameraPreviewGenerator::transform(Func& output, Func input, Func m) {
    Expr ir = input(v_x, v_y, 0);
    Expr ig = input(v_x, v_y, 1);
    Expr ib = input(v_x, v_y, 2);

    // Color correct
    Expr r = cast<float16_t>(m(0, 0)) * ir + cast<float16_t>(m(1, 0)) * ig + cast<float16_t>(m(2, 0)) * ib;
    Expr g = cast<float16_t>(m(0, 1)) * ir + cast<float16_t>(m(1, 1)) * ig + cast<float16_t>(m(2, 1)) * ib;
    Expr b = cast<float16_t>(m(0, 2)) * ir + cast<float16_t>(m(1, 2)) * ig + cast<float16_t>(m(2, 2)) * ib;
    
    output(v_x, v_y, v_c) = select(v_c == 0, clamp(r, cast<float16_t>(0.0f), cast<float16_t>(1.0f)),
                                   v_c == 1, clamp(g, cast<float16_t>(0.0f), cast<float16_t>(1.0f)),
                                             clamp(b, cast<float16_t>(0.0f), cast<float16_t>(1.0f)));
}

void CameraPreviewGenerator::pyramidUp(Func& output, Func& intermediate, Func input) {
    Func blurX("blurX");
    Func blurY("blurY");

    // Insert zeros and expand by factor of 2 in both dims
    Func expandedX;
    Func expanded("expanded");

    expandedX(v_x, v_y, v_c) = select((v_x % 2)==0, input(v_x/2, v_y, v_c), 0);
    expanded(v_x, v_y, v_c)  = select((v_y % 2)==0, expandedX(v_x, v_y/2, v_c), 0);

    blurX(v_x, v_y, v_c) =
        (       expanded(v_x - 1, v_y, v_c) +
            2 * expanded(v_x,     v_y, v_c) +
                expanded(v_x + 1, v_y, v_c)
        ) * 0.25f;

    blurY(v_x, v_y, v_c) =
        (       blurX(v_x, v_y - 1, v_c) +
            2 * blurX(v_x, v_y,     v_c) +
                blurX(v_x, v_y + 1, v_c)
        ) * 0.25f;

    intermediate = blurX;
    output(v_x, v_y, v_c) = cast<float16_t>(4 * blurY(v_x, v_y, v_c));
}

void CameraPreviewGenerator::pyramidDown(Func& output, Func& intermediate, Func input) {
    Func blurX, blurY;

    blurX(v_x, v_y, v_c) =
        (       input(v_x - 1, v_y, v_c) +
            2 * input(v_x,     v_y, v_c) +
                input(v_x + 1, v_y, v_c)
        ) * 0.25f;

    blurY(v_x, v_y, v_c) =
        (       blurX(v_x, v_y - 1,   v_c) +
            2 * blurX(v_x, v_y,       v_c) +
                blurX(v_x, v_y + 1,   v_c)
        ) * 0.25f;

    intermediate = blurX;
    output(v_x, v_y, v_c) = cast<float16_t>(blurY(v_x * 2, v_y * 2, v_c));
}

vector<pair<Func, Func>> CameraPreviewGenerator::buildPyramid(Func input, int maxlevel) {
    vector<pair<Func, Func>> pyramid;

    for(int level = 1; level <= maxlevel; level++) {
        Func pyramidDownOutput("pyramidDownLvl" + std::to_string(level));
        Func pyramidDownIntermediate("pyramidDownIntermediateLvl" + std::to_string(level));
        
        Func inClamped;

        if(level == 1) {
            inClamped = BoundaryConditions::repeat_edge(input, { {0, width}, {0, height} } );

            pyramid.push_back(std::make_pair(inClamped, inClamped));
        }
        else {
            inClamped = BoundaryConditions::repeat_edge(pyramid[level - 1].second, { {0, width >> (level-1)}, {0, height >> (level-1)} } );
        }

        pyramidDown(pyramidDownOutput, pyramidDownIntermediate, inClamped);
        
        pyramid.push_back(std::make_pair(pyramidDownIntermediate, pyramidDownOutput));
    }

    return pyramid;
}

Func CameraPreviewGenerator::tonemap(Func input, Expr gain, Expr gamma, Expr variance) {
    vector<pair<Func, Func>> tonemapPyramid;
    vector<pair<Func, Func>> weightsPyramid;
    
    Expr HALF_0_0 = cast<float16_t>(0.0f);
    Expr HALF_0_5 = cast<float16_t>(0.5f);
    Expr HALF_1_0 = cast<float16_t>(1.0f);

    // Create two exposures
    Func exposures, weights, weightsNormalized;

    Expr ia = pow(clamp(input(v_x, v_y, 0), HALF_0_0, HALF_1_0), cast<float16_t>(1.0f/gamma));
    Expr ib = pow(clamp(input(v_x, v_y, 0) * cast<float16_t>(gain), HALF_0_0, HALF_1_0), cast<float16_t>(1.0f/gamma));

    exposures(v_x, v_y, v_c) = select(v_c == 0, ia, ib);

    Expr wa = exposures(v_x, v_y, v_c) - HALF_0_5;
    Expr wb = -(wa * wa) * cast<float16_t>((1.0f / (2.0f * variance * variance)));

    weights(v_x, v_y, v_c) = cast<float16_t>(exp(wb));
    weightsNormalized(v_x, v_y, v_c) = weights(v_x, v_y, v_c) / (cast<float16_t>(1e-5f) + weights(v_x, v_y, 0) + weights(v_x, v_y, 1));

    // Create pyramid input
    tonemapPyramid = buildPyramid(exposures, tonemap_levels);
    weightsPyramid = buildPyramid(weightsNormalized, tonemap_levels);

    int tx = 8;
    int ty = 4;

    if(downscale_factor == 4) {
        tx = ty = 4;
    }

    if (get_target().has_gpu_feature()) {
        tonemapPyramid[0].first.in(tonemapPyramid[1].first)
            .compute_at(tonemapPyramid[1].second, v_x)
            .reorder(v_c, v_x, v_y)
            .gpu_threads(v_x, v_y);
        
        weightsPyramid[0].first.in(weightsPyramid[1].first)
            .compute_at(weightsPyramid[1].second, v_x)
            .reorder(v_c, v_x, v_y)
            .gpu_threads(v_x, v_y);

        for(int level = 1; level < tonemap_levels; level++) {
            tonemapPyramid[level].first
                .reorder(v_c, v_x, v_y)
                .compute_at(tonemapPyramid[level].second, v_x)
                .gpu_threads(v_x, v_y);

            tonemapPyramid[level].second                
                .compute_root()
                .reorder(v_c, v_x, v_y)
                .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);

            weightsPyramid[level].first
                .reorder(v_c, v_x, v_y)
                .compute_at(weightsPyramid[level].second, v_x)
                .gpu_threads(v_x, v_y);

            weightsPyramid[level].second
                .compute_root()
                .reorder(v_c, v_x, v_y)
                .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);
        }
    }
    else {
        // TODO: Better schedule
        for(int level = 0; level < tonemap_levels; level++) {
            tonemapPyramid[level].first.compute_root();
            tonemapPyramid[level].second.compute_root();

            weightsPyramid[level].first.compute_root();
            weightsPyramid[level].second.compute_root();
        }
    }

    vector<Func> laplacianPyramid, combinedPyramid;
    
    //
    // Create laplacian pyramid
    //
    
    for(int level = 0; level < tonemap_levels; level++) {
        Func up("laplacianUpLvl" + std::to_string(level));
        Func upIntermediate("laplacianUpIntermediateLvl" + std::to_string(level));
        Func laplacian("laplacianLvl" + std::to_string(level));

        pyramidUp(up, upIntermediate, tonemapPyramid[level + 1].second);
        
        laplacian(v_x, v_y, v_c) = tonemapPyramid[level].second(v_x, v_y, v_c) - up(v_x, v_y, v_c);

        // Skip first level
        if (get_target().has_gpu_feature()) {
            if(level > 0) {
                up
                    .reorder(v_c, v_x, v_y)
                    .compute_at(laplacian, v_x)
                    .gpu_threads(v_x, v_y);

                upIntermediate
                    .reorder(v_c, v_x, v_y)
                    .compute_at(laplacian, v_x)
                    .gpu_threads(v_x, v_y);

                laplacian
                    .compute_root()
                    .reorder(v_c, v_x, v_y)
                    .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);
            }
        }
        else {
            // TODO: Better schedule
            up.compute_root();
            upIntermediate.compute_root();
            laplacian.compute_root();
        }

        laplacianPyramid.push_back(laplacian);
    }

    laplacianPyramid.push_back(tonemapPyramid[tonemap_levels].second);

    //
    // Combine pyramids
    //
    
    for(int level = 0; level <= tonemap_levels; level++) {
        Func result("resultLvl" + std::to_string(level));

        result(v_x, v_y, v_c) =
            (laplacianPyramid[level](v_x, v_y, 0) * weightsPyramid[level].second(v_x, v_y, 0)) +
            (laplacianPyramid[level](v_x, v_y, 1) * weightsPyramid[level].second(v_x, v_y, 1));
        
        combinedPyramid.push_back(result);
    }

    //
    // Create output pyramid
    //
    
    vector<Func> outputPyramid;

    for(int level = tonemap_levels; level > 0; level--) {
        Func up("outputUpLvl" + std::to_string(level));
        Func upIntermediate("outputUpIntermediateLvl" + std::to_string(level));
        Func outputLvl("outputLvl" + std::to_string(level));

        if(level == tonemap_levels) {
            pyramidUp(up, upIntermediate, combinedPyramid[level]);
        }
        else {
            pyramidUp(up, upIntermediate, outputPyramid[outputPyramid.size() - 1]);
            
        }

        outputLvl(v_x, v_y, v_c) = combinedPyramid[level - 1](v_x, v_y, v_c) + up(v_x, v_y, v_c);

        // Skip last level
        if (get_target().has_gpu_feature()) {
            upIntermediate
                .reorder(v_c, v_x, v_y)
                .compute_at(outputLvl, v_x)
                .unroll(v_c)
                .gpu_threads(v_x, v_y);

            up
                .reorder(v_c, v_x, v_y)
                .compute_at(outputLvl, v_x)
                .unroll(v_c)
                .gpu_threads(v_x, v_y);

            outputLvl
                .compute_root()
                .reorder(v_c, v_x, v_y)
                .unroll(v_c)
                .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);
        }
        else {
            // TODO: Better schedule
            upIntermediate.compute_root();
            up.compute_root();
            outputLvl.compute_root();
        }

        outputPyramid.push_back(outputLvl);
    }

    // Inverse gamma correct tonemapped result
    Func tonemapped("tonemapped");

    tonemapped(v_x, v_y) = pow(clamp(outputPyramid[tonemap_levels - 1](v_x, v_y, 0), 0.0f, 1.0f), gamma);
    
    // Create output RGB image
    Expr uvScale = tonemapped(v_x, v_y) / (cast<float16_t>(1e-3f) + input(v_x, v_y, 0));

    Expr U = uvScale * (input(v_x, v_y, 1) - HALF_0_5) + HALF_0_5;
    Expr V = uvScale * (input(v_x, v_y, 2) - HALF_0_5) + HALF_0_5;
    
    Func output("tonemapOutput");

    output(v_x, v_y, v_c) =
        cast<float16_t>(
            select(v_c == 0, tonemapped(v_x, v_y),
                   v_c == 1, U,
                             V));

    return output;
}

void CameraPreviewGenerator::generate() {
    Func yuvOutput{"yuvOutput"};
    Func linear{"linear"};
    Func clampedInput{"clampedInput"};
    Func colorCorrected{"colorCorrected"};
    Func colorCorrectedYuv{"colorCorrectedYuv"};
    Func tonemapOutputRgb{"tonemapOutputRgb"};
    Func gammaContrastLut{"gammaContrastLut"};
    Func inputRepeated{"inputRepeated"};
    Func scaledShadingMap[4] { Func("scaledShadingMap[0]"), Func("scaledShadingMap[1]"), Func("scaledShadingMap[2]"), Func("scaledShadingMap[3]") };
    Func shadingMapInput{"shadingMapInput"};
    Func bayer{"bayer"};
    Func rawInput{"rawInput"};
    Func downscaled{"downscaled"};
    Func downscaledTemp{"downscaledTemp"};
    Func demosaicInput("demosaicInput");
    Func blackLevelFunc{"blackLevelFunc"};
    Func linearFunc{"linearFunc"};
    Func asShotFunc{"asShotFunc"};
    Func wbOffsetFunc{"wbOffsetFunc"};
    Func demosaiced{"demosaiced"};

    inputRepeated(v_i) = cast<uint16_t>(input(v_i));

    for(int c = 0; c < 4; c++) {
        linearScale16(scaledShadingMap[c], shadingMap[c], shadingMap[c].width(), shadingMap[c].height(), width, height);
    }

    if(pixel_format == static_cast<int>(RawFormat::RAW10)) {
        Expr X = (v_x / 4) * 4;
        Expr xoffset = (v_y * stride) + 10*X/8;
        
        Expr p = v_x - X;
        Expr shift = p * 2;

        bayer(v_x, v_y) = (inputRepeated(xoffset + p) << 2) | ((inputRepeated(xoffset + 4) >> shift) & 0x03);
    }
    else if(pixel_format == static_cast<int>(RawFormat::RAW12)) {
        Expr X = (v_x / 2) * 2;
        Expr xoffset = (v_y*stride) + 12*X/8;

        Expr p = v_x - X;
        Expr shift = p * 4;

        bayer(v_x, v_y) = (inputRepeated(xoffset + p)) << 4 | ((inputRepeated(xoffset + 2) >> shift) & 0x0F);

    }    
    else if(pixel_format == static_cast<int>(RawFormat::RAW16)) {
        Expr offset = (v_y*stride) + (v_x*2);

        bayer(v_x, v_y) = inputRepeated(offset) | (inputRepeated(offset + 1) << 8);
    }
    else {
        throw std::runtime_error("invalid pixel format");
    }

    rawInput(v_x, v_y, v_c) = select(
        v_c == 0, bayer(v_x*2,      v_y*2),
        v_c == 1, bayer(v_x*2 + 1,  v_y*2),
        v_c == 2, bayer(v_x*2,      v_y*2 + 1),
                  bayer(v_x*2 + 1,  v_y*2 + 1));

    clampedInput(v_x, v_y, v_c) = rawInput(clamp(v_x, 0, width*downscale_factor-1), clamp(v_y, 0, height*downscale_factor-1), v_c);

    demosaicInput(v_x, v_y, v_c) =
        select(sensorArrangement == static_cast<int>(SensorArrangement::RGGB),
                select( v_c == 0, clampedInput(v_x, v_y, 0),
                        v_c == 1, clampedInput(v_x, v_y, 1),
                                  clampedInput(v_x, v_y, 3) ),

            sensorArrangement == static_cast<int>(SensorArrangement::GRBG),
                select( v_c == 0, clampedInput(v_x, v_y, 1),
                        v_c == 1, clampedInput(v_x, v_y, 0),
                                  clampedInput(v_x, v_y, 2) ),

            sensorArrangement == static_cast<int>(SensorArrangement::GBRG),
                select( v_c == 0, clampedInput(v_x, v_y, 2),
                        v_c == 1, clampedInput(v_x, v_y, 0),
                                  clampedInput(v_x, v_y, 1) ),

                select( v_c == 0, clampedInput(v_x, v_y, 3),
                        v_c == 1, clampedInput(v_x, v_y, 1),
                                  clampedInput(v_x, v_y, 0) ) );

    shadingMapInput(v_x, v_y, v_c) =
        mux(v_c, { scaledShadingMap[0](v_x, v_y), scaledShadingMap[1](v_x, v_y), scaledShadingMap[3](v_x, v_y) } );

    downscaled = downscale(demosaicInput, downscaledTemp);

    Func flippedDownscaled;

    flippedDownscaled(v_x, v_y, v_c) =
        select(flipped, downscaled(width - v_x, v_y, v_c),
                        downscaled(v_x, v_y, v_c));

    blackLevelFunc(v_c) = cast<float16_t>(
                            select( v_c == 0, blackLevel[0],
                                    v_c == 1, blackLevel[1],
                                              blackLevel[3]) );

    linearFunc(v_c) = cast<float16_t>(
                      select(   v_c == 0, 1.0f / (whiteLevel - blackLevelFunc(v_c)),
                                v_c == 1, 1.0f / (whiteLevel - blackLevelFunc(v_c)),
                                          1.0f / (whiteLevel - blackLevelFunc(v_c))) );

    blackLevelFunc.compute_root().unroll(v_c);
    linearFunc.compute_root().unroll(v_c);

    linear(v_x, v_y, v_c) = cast<float16_t>((flippedDownscaled(v_x, v_y, v_c) - blackLevelFunc(v_c)) * linearFunc(v_c));

    asShotFunc(v_c) = mux(v_c, { asShotVector[0], asShotVector[1], asShotVector[2] } );
    wbOffsetFunc(v_c) = mux(v_c, { wbOffset[0], wbOffset[1], wbOffset[2] } );

    demosaiced(v_x, v_y, v_c) = cast<float16_t>(wbOffsetFunc(v_c)) * shadingMapInput(v_x, v_y, v_c) * clamp(linear(v_x, v_y, v_c), cast<float16_t>(0.0f), cast<float16_t>(asShotFunc(v_c)));

    transform(colorCorrected, demosaiced, cameraToSrgb);

    Expr HALF_0_0 = cast<float16_t>(0.0f);
    Expr HALF_0_5 = cast<float16_t>(0.5f);
    Expr HALF_1_0 = cast<float16_t>(1.0f);
    Expr HALF_2_0 = cast<float16_t>(2.0f);
    Expr HALF_4_0 = cast<float16_t>(4.0f);

    // Move to YUV space
    Expr R = colorCorrected(v_x, v_y, 0);
    Expr G = colorCorrected(v_x, v_y, 1);
    Expr B = colorCorrected(v_x, v_y, 2);

    Expr Y = HALF_YUV_R*R + HALF_YUV_G*G + HALF_YUV_B*B;
    Expr U = HALF_0_5 * (B - Y) / (HALF_1_0 - HALF_YUV_B) + HALF_0_5;
    Expr V = HALF_0_5 * (R - Y) / (HALF_1_0 - HALF_YUV_R) + HALF_0_5;

    yuvOutput(v_x, v_y, v_c) = cast<float16_t>(
        select(v_c == 0, Y,
               v_c == 1, U,
                         V));

    // Tonemap
    Func tonemapOutput("tonemapOutput");

    tonemapOutput = tonemap(yuvOutput, shadows, gamma, tonemapVariance);

    // Back to RGB
    Y = cast<float16_t>(tonemapOutput(v_x, v_y, 0));
    U = cast<float16_t>(tonemapOutput(v_x, v_y, 1));
    V = cast<float16_t>(tonemapOutput(v_x, v_y, 2));

    R = Y + HALF_2_0*(V - HALF_0_5) * (HALF_1_0 - HALF_YUV_R);
    G = Y - HALF_2_0*(U - HALF_0_5) * (HALF_1_0 - HALF_YUV_B) * HALF_YUV_B / HALF_YUV_G - HALF_2_0*(V - HALF_0_5) * (HALF_1_0 - HALF_YUV_R) * HALF_YUV_R / HALF_YUV_G;
    B = Y + HALF_2_0*(U - HALF_0_5) * (HALF_1_0 - HALF_YUV_B);
    
    // Apply saturation in HSL space
    Expr maxRgb = max(R, G, B);
    Expr minRgb = min(R, G, B);

    Expr P = (minRgb + maxRgb) / cast<float16_t>(2.0f);

    Expr outR = (R - P) * cast<float16_t>(saturation) + P;
    Expr outG = (G - P) * cast<float16_t>(saturation) + P;
    Expr outB = (B - P) * cast<float16_t>(saturation) + P;

    tonemapOutputRgb(v_x, v_y, v_c) = cast<float16_t>(
        select(v_c == 0, clamp(outR, HALF_0_0, HALF_1_0),
               v_c == 1, clamp(outG, HALF_0_0, HALF_1_0),
                         clamp(outB, HALF_0_0, HALF_1_0)));

    // Finalize
    Expr b = HALF_2_0 - pow(HALF_2_0, cast<float16_t>(contrast));
    Expr a = HALF_2_0 - HALF_2_0 * b;

    // Gamma correct
    Expr g = pow(tonemapOutputRgb(v_x, v_y, v_c), HALF_1_0 / cast<float16_t>(gamma));

    // Apply a piecewise quadratic contrast curve
    Expr h0 = select(g > HALF_0_5,
                     HALF_1_0 - (a*(HALF_1_0-g)*(HALF_1_0-g) + b*(HALF_1_0-g)),
                     a*g*g + b*g);

    // Apply blacks/white point
    Expr h1 = (h0 - cast<float16_t>(blacks)) / cast<float16_t>(whitePoint);

    output(v_x, v_y, v_c) =
        select(v_c < 3,
            cast<uint8_t>(clamp(h1 * cast<float16_t>(255) + HALF_0_5, cast<float16_t>(0), cast<float16_t>(255))),
            255);

    // Output interleaved
    output
        .dim(0).set_stride(4)
        .dim(2).set_stride(1);

    if (get_target().has_gpu_feature()) {
        int tx = 8;
        int ty = 4;

        if(downscale_factor == 4) {
            tx = ty = 4;
        }

        clampedInput
            .reorder(v_c, v_x, v_y)
            .compute_at(downscaled, v_x)            
            .gpu_threads(v_x, v_y);

        downscaledTemp
            .reorder(v_c, v_x, v_y)
            .compute_at(downscaled, v_x)
            .vectorize(v_c)
            .gpu_threads(v_x, v_y);            

        downscaled
            .compute_root()
            .reorder(v_c, v_x, v_y)
            .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);
     
        yuvOutput
            .compute_root()
            .reorder(v_c, v_x, v_y)
            .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);


        output
            .bound(v_c, 0, 4)
            .compute_root()
            .reorder(v_c, v_x, v_y)
            .unroll(v_c)
            .gpu_tile(v_x, v_y, v_xi, v_yi, tx, ty);
    }
    else {
        // TODO: Better schedule
        clampedInput.compute_root();
        downscaledTemp.compute_root();
        downscaled.compute_root();
        output.compute_root();
    }
}

HALIDE_REGISTER_GENERATOR(CameraPreviewGenerator, camera_preview_generator)
HALIDE_REGISTER_GENERATOR(CameraVideoPreviewGenerator, camera_video_preview_generator)
