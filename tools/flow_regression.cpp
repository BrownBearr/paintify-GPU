// Direct GPU readback regression for Follow image. Run from the project root.
// Builds against the same gl_util, GLFW and GLAD as the app; no image heuristics.
#include "gl_util.h"
#include "params.h"
#include "style.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr int W = 64, H = 64;
constexpr double PI = 3.141592653589793;
int failures = 0;
void require(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
std::vector<float> read(GLuint tex) {
    std::vector<float> out(W * H * 4);
    glGetTextureImage(tex, 0, GL_RGBA, GL_FLOAT, GLsizei(out.size() * sizeof(float)), out.data());
    return out;
}
float maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return std::numeric_limits<float>::infinity();
        d = std::max(d, std::abs(a[i] - b[i]));
    }
    return d;
}
double angle(const std::vector<float>& v) {
    const int i = ((H / 2) * W + W / 2) * 4;
    return std::atan2(v[i], -v[i + 1]) * 180 / PI;
}
double axialDistance(double a, double b) {
    return std::abs(std::remainder(a - b, 180.0));
}

bool loadCurlProbe(const std::string& shaderPath, GLuint& program) {
    std::string err;
    std::string source = glu::loadShaderSource(shaderPath, &err);
    const std::string entry = "void main() {";
    const size_t mainAt = source.rfind(entry);
    if (source.empty() || mainAt == std::string::npos) {
        std::fprintf(stderr, "curl probe source injection failed: %s\n", err.c_str());
        return false;
    }
    // Keep production curlAngle/fbm calculations. Instrument its actual
    // stagnation branch and scan bounded, integer-safe coordinates because
    // shader compilers may reassociate (p +/- 1) / scale.
    source.resize(mainAt);
    const std::string curlDecl = "float curlAngle(vec2 p) {";
    const size_t curlAt = source.find(curlDecl);
    const std::string zeroBranch = "    if (dx == 0.0 && dy == 0.0) return 0.0; // curl stagnation point";
    const std::string atanReturn = "    return atan(-dx, dy);     // streamline of the stream function: (dpsi/dy, -dpsi/dx)";
    if (curlAt == std::string::npos) {
        std::fprintf(stderr, "curl probe instrumentation anchor missing\n");
        return false;
    }
    const std::string outputDecl = "layout(rgba16f, binding = 0) uniform writeonly image2D uProbeOut;\n";
    source.insert(curlAt, outputDecl);
    const size_t zeroAt = source.find(zeroBranch, curlAt + outputDecl.size());
    const size_t atanAt = source.find(atanReturn, curlAt + outputDecl.size());
    if (atanAt == std::string::npos) {
        std::fprintf(stderr, "curl probe angle instrumentation anchor missing\n");
        return false;
    }
    if (zeroAt != std::string::npos)
        source.replace(zeroAt, zeroBranch.size(), "    if (dx == 0.0 && dy == 0.0) { imageStore(uProbeOut, ivec2(gl_GlobalInvocationID.xy), vec4(dx, dy, 0.0, 1.0)); return 0.0; }");
    const size_t atanAt2 = source.find(atanReturn, curlAt + outputDecl.size());
    source.replace(atanAt2, atanReturn.size(), "    float a = atan(-dx, dy); imageStore(uProbeOut, ivec2(gl_GlobalInvocationID.xy), vec4(dx, dy, a, (dx == 0.0 && dy == 0.0) ? 1.0 : 0.0)); return a;");
    source += R"(
void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    // p ranges from 2^20 through 2^34. With curlScale=110 and three octaves,
    // gnoise's float-to-int lattice indices stay inside signed 32-bit range.
    vec2 p = exp2(vec2(20.0 + float(id.x % 57u) * 0.25, 20.0 + float(id.y % 57u) * 0.25));
    curlAngle(p);
}
)";
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &n);
        std::string log(size_t(std::max(n, 1)), '\0');
        glGetShaderInfoLog(shader, n, nullptr, log.data());
        std::fprintf(stderr, "curl probe compile failed: %s\n", log.c_str());
        glDeleteShader(shader);
        return false;
    }
    program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glDeleteShader(shader);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &n);
        std::string log(size_t(std::max(n, 1)), '\0');
        glGetProgramInfoLog(program, n, nullptr, log.data());
        std::fprintf(stderr, "curl probe link failed: %s\n", log.c_str());
        glDeleteProgram(program);
        program = 0;
        return false;
    }
    return true;
}
}

struct Vec { double x,y; Vec operator+(Vec b)const{return{x+b.x,y+b.y};} Vec operator*(double s)const{return{x*s,y*s};} };
Vec axis(double a){return{std::cos(2*a),std::sin(2*a)};}
uint32_t mixHash(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
uint32_t noiseHash(int x,int y,uint32_t s){return mixHash(uint32_t(x)*0x8da6b343u^uint32_t(y)*0xd8163841u^s*0xcb1ab31fu);}
float noise01(int x,int y,uint32_t s){return float(noiseHash(x,y,s)&0xffffffu)/16777216.f;}
float noise(float x,float y,uint32_t s){
    int ix=int(std::floor(x)),iy=int(std::floor(y));float fx=x-ix,fy=y-iy;
    auto grad=[&](int dx,int dy){float a=noise01(ix+dx,iy+dy,s)*6.2831853f;return std::cos(a)*(fx-dx)+std::sin(a)*(fy-dy);};
    auto fade=[](float t){return t*t*t*(t*(t*6-15)+10);};float u=fade(fx),v=fade(fy);
    float a=grad(0,0),b=grad(1,0),c=grad(0,1),d=grad(1,1);return((a+(b-a)*u)*(1-v)+(c+(d-c)*u)*v)*1.414f;
}
float fractal(float x,float y,uint32_t s){
    float total=0,amp=1,norm=0,fr=1;for(int o=0;o<3;++o){total+=amp*noise(x*fr+o*19.19f,y*fr+o*7.31f,s+o*101u);norm+=amp;amp*=.5f;fr*=2;}return total/norm;
}
double cpuCurl(float x,float y,float sc){
    sc=std::max(sc,4.f);float dx=fractal((x+1)/sc,y/sc,601u)-fractal((x-1)/sc,y/sc,601u);
    float dy=fractal(x/sc,(y+1)/sc,601u)-fractal(x/sc,(y-1)/sc,601u);return std::atan2(-dx,dy);
}
double cpuFallback(float x,float y,const StyleParams&s){return s.fallbackMode>.5f?(s.fallbackMode<1.5f?cpuCurl(x,y,s.curlScale):0):s.fallbackAngle*PI/180;}
// Scalar CPU references check per-pixel geometry, not merely changed output.
Vec expectedAxis(int x,int y,const std::vector<float>&f,const StyleParams&s){
    size_t i=(y*W+x)*4;double mag=f[i+2],strength=s.fieldMix;
    auto at=[&](int sx,int sy){size_t k=(std::clamp(sy,0,H-1)*W+std::clamp(sx,0,W-1))*4;return std::atan2(f[k],-f[k+1]);};
    double px=x+.5,py=y+.5;Vec acc=axis((f[i]!=0||f[i+1]!=0)?at(x,y):cpuFallback(float(px),float(py),s));
    int mode=int(s.fieldMode+.5f);double conf=std::clamp(mag/std::max(double(s.coherenceMin),1e-4),0.,1.);
    if(mode==1){
        double dx=px-s.vortexX*W,dy=(py-s.vortexY*H)/1.1,r=std::hypot(dx,dy)/std::max(W,H);
        double w=strength*(.05+.9*std::exp(-std::pow(r/std::max(double(s.vortexRadius),1e-3),2)));
        acc=acc*(1-w)+axis(std::atan2(dy,dx)+PI/2+s.vortexSpiral)*w;
    }else if(mode==2){
        double cell=std::max(double(s.patchCell),2.),qx=px/cell,qy=py/cell,best=1e9,spx=0,spy=0;
        int ix=int(std::floor(qx)),iy=int(std::floor(qy));uint32_t id=0;
        for(int cy=iy-1;cy<=iy+1;++cy)for(int cx=ix-1;cx<=ix+1;++cx){
            double fx=cx+noise01(cx,cy,701u),fy=cy+noise01(cx,cy,701u^0x5bd1e995u),d=std::hypot(fx-qx,fy-qy);
            if(d<best){best=d;spx=fx*cell;spy=fy*cell;id=noiseHash(cx,cy,701u^0x1234567u);}}
        int sx=int(spx),sy=int(spy);size_t k=(std::clamp(sy,0,H-1)*W+std::clamp(sx,0,W-1))*4;
        double cs=std::clamp(f[k+2]/std::max(double(s.coherenceMin),1e-4),0.,1.);
        Vec sd=axis(at(sx,sy))*cs+axis(cpuFallback(float(sx),float(sy),s))*(1-cs);
        double pa=.5*std::atan2(sd.y,sd.x)+(double(mixHash(id)&0xffffffu)/16777216.*2-1)*s.patchJitter;
        if(s.patchQuant>0)pa=std::floor(pa/s.patchQuant+.5)*s.patchQuant;acc=acc*(1-strength)+axis(pa)*strength;
    }else if(mode==3){double w=strength*(1-conf);acc=acc*(1-w)+axis(cpuCurl(float(px),float(py),s.curlScale))*w;
    }else if(mode==4){
        float lam=std::max(s.waveLen,4.f);double ph=2*PI*(px*std::cos(s.waveAngle)+py*std::sin(s.waveAngle))/lam;
        double wob=2*fractal(float(px)/(lam*2),float(py)/(lam*2),611u);
        double a=s.waveAngle+s.waveAmp*std::sin(ph+wob)*.6,w=strength*(1-conf);acc=acc*(1-w)+axis(a)*w;
    }else if(mode==5)acc=acc*(1-strength)+axis(s.fallbackAngle*PI/180)*strength;
    return acc;
}
int main(int argc, char** argv) {
    const char* baselineDir = nullptr;
    const char* directionShader = "stylefield.comp";
    for(int arg=1;arg<argc;++arg){
        if(arg+1<argc&&std::string(argv[arg])=="--baseline-dir")baselineDir=argv[++arg];
        else if(arg+1<argc&&std::string(argv[arg])=="--direction-shader")directionShader=argv[++arg];
        else{std::fprintf(stderr,"usage: flow_regression [--baseline-dir <shaders>] [--direction-shader <shader relative to shaders/>]\n");return 2;}
    }
    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(32, 32, "flow regression", nullptr, nullptr);
    if (!window) return 2;
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 2;
    std::printf("GPU: %s\n", glGetString(GL_RENDERER));
    glu::Program features, baseline, fixed, etf;
    if (!features.loadCompute("features.comp") ||
        !etf.loadCompute("etf.comp") ||
        !fixed.loadCompute(directionShader)) return 2;
    GLuint curlProbe = 0;
    if (!loadCurlProbe(directionShader, curlProbe)) return 2;
    if (baselineDir && !baseline.loadCompute(std::string("../") + baselineDir + "/stylefield.comp")) return 2;
    TuningParams params;
    params.imgW = W; params.imgH = H;
    params.texelW = 1.f / W; params.texelH = 1.f / H;
    const GLuint tuning = glu::createBuffer(GL_UNIFORM_BUFFER, sizeof(params), &params, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, tuning);
    StyleParams style;
    style.enabled = style.brushModel = 1.f;
    const GLuint styling = glu::createBuffer(GL_UNIFORM_BUFFER, sizeof(style), &style, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, styling);
    const unsigned zero = 0;
    const GLuint etfMax = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, sizeof(zero), &zero, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, etfMax);
    const GLuint src = glu::createTexture2D(W, H, GL_RGBA32F);
    const GLuint grad = glu::createTexture2D(W, H, GL_RGBA16F);
    const GLuint oldField = glu::createTexture2D(W, H, GL_RGBA16F);
    const GLuint newField = glu::createTexture2D(W, H, GL_RGBA16F);
    auto dispatch = [&](glu::Program& program, GLuint input, GLuint output) {
        program.use();
        glBindTextureUnit(0, input);
        glBindImageTexture(0, output, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(W / 16, H / 16, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    };
    auto uploadStyle = [&]() { glNamedBufferSubData(styling, 0, sizeof(style), &style); };
    int deepDispatches=0;size_t deepPixels=0;
    auto deepOne=[&](GLuint input,const std::vector<float>& raw,const char* label){
        uploadStyle();dispatch(fixed,input,newField);const auto output=read(newField);++deepDispatches;deepPixels+=W*H;
        bool finite=true,exact=true,magnitude=true;double worstAngle=0;int compared=0;
        const int mode=int(style.fieldMode+.5f);const double strength=mode?style.fieldMix:0;
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){size_t i=(y*W+x)*4;
            for(int k=0;k<4;++k){finite&=std::isfinite(output[i+k]);exact&=raw[i+k]==output[i+k];}
            double pm=std::max(double(raw[i+2]),double(style.minFieldMag));
            double expectedMag=raw[i+2]*(1-strength)+pm*strength;
            double expectedLen=std::hypot(raw[i],raw[i+1])*(1-strength)+pm*strength;
            magnitude&=std::abs(output[i+2]-expectedMag)<.001*std::max(expectedMag,.001)+.000001;
            magnitude&=std::abs(std::hypot(output[i],output[i+1])-expectedLen)<.0015*std::max(expectedLen,.001)+.000001;
            if(expectedLen<1e-7)continue;
            Vec expected=expectedAxis(x,y,raw,style);
            // A perfectly opposed axial blend has no unique average. Separate
            // equivalent-axis and tie-fallback assertions exercise that case.
            if(std::hypot(expected.x,expected.y)<1e-5)continue;
            if(mode==1&&std::hypot(x+.5-style.vortexX*W,y+.5-style.vortexY*H)<1e-7)continue;
            double ea=.5*std::atan2(expected.y,expected.x)*180/PI;
            double oa=std::atan2(output[i],-output[i+1])*180/PI;
            worstAngle=std::max(worstAngle,axialDistance(ea,oa));++compared;
        }
        char name[256];std::snprintf(name,sizeof(name),"deep %s mode%d strength%.4g finite/magnitude/reference (worst %.5f deg, %d pixels)",label,mode,style.fieldMix,worstAngle,compared);
        // CPU/GPU sin/finite-difference noise evaluation varies slightly near
        // stationary points. Curl's angular tolerance is wider for that reason.
        require(finite&&magnitude&&(strength==0?exact:worstAngle<(mode==3?1.:.15)),name);
    };
    auto deepSuite=[&](GLuint input,const char* label){
        const auto raw=read(input);
        for(int mode=0;mode<6;++mode)for(float amount:{0.f,.0001f,.25f,.5f,.75f,1.f}){
            style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=float(mode);style.fieldMix=amount;
            deepOne(input,raw,label);
        }
    };    auto checkFilteredSource = [&](GLuint input, const char* label) {
        const auto original = read(input);
        style = StyleParams{}; style.enabled = style.brushModel = 1.f;
        uploadStyle(); dispatch(fixed, input, newField);
        require(maxDifference(original, read(newField)) == 0.f, label);
        for (int mode = 1; mode <= 5; ++mode) {
            style.fieldMode = float(mode); style.fieldMix = 0; uploadStyle();
            dispatch(fixed, input, newField);
            require(maxDifference(original, read(newField)) == 0.f, "filtered pattern strength zero exact source");
            style.fieldMix = .0001f; uploadStyle(); dispatch(fixed, input, newField);
            require(maxDifference(original, read(newField)) < .0006f, "filtered epsilon vector convergence");
        }
    };
    struct Case { const char* name; float dx, dy; double expected; };
    const Case cases[] = {
        {"ramp +x", .001f, 0, 90}, {"ramp -x", -.001f, 0, 90},
        {"ramp +y", 0, .001f, 0}, {"ramp -y", 0, -.001f, 0},
        {"ramp diagonal", .001f, .001f, -45}, {"flat", 0, 0, 0}
    };
    for (const auto& c : cases) {
        std::vector<float> pixels(W * H * 4, 1.f);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float value = .5f + (x - W / 2) * c.dx + (y - H / 2) * c.dy;
            for (int k = 0; k < 3; ++k) pixels[(y * W + x) * 4 + k] = value;
        }
        glTextureSubImage2D(src, 0, 0, 0, W, H, GL_RGBA, GL_FLOAT, pixels.data());
        features.use(); glUniform1i(features.uniform("uPass"), 0);
        dispatch(features, src, grad);
        style = StyleParams{}; style.enabled = style.brushModel = 1.f; uploadStyle();
        if (baselineDir) dispatch(baseline, grad, oldField);
        dispatch(fixed, grad, newField);
        const auto raw = read(grad), after = read(newField);
        const bool flat = c.dx == 0 && c.dy == 0;
        deepSuite(grad,c.name);
        std::printf("%s: fixed %.4f deg, source %.4f deg, field max delta %.8f",
                    c.name, angle(after), angle(raw), maxDifference(raw, after));
        if (baselineDir) std::printf(", baseline %.4f deg", angle(read(oldField)));
        std::printf("\n");
        require(maxDifference(raw, after) == 0.f, c.name);
        if (!flat) require(axialDistance(angle(after), c.expected) < .02, "analytic tangent");
        else require(*std::max_element(after.begin(), after.end()) == 0.f, "flat remains directionless");
        for (int mode = 1; mode <= 5; ++mode) {
            style.fieldMode = float(mode); style.fieldMix = 1.f; uploadStyle();
            if (baselineDir) dispatch(baseline, grad, oldField);
            dispatch(fixed, grad, newField);
            const auto full = read(newField);
            require(std::all_of(full.begin(), full.end(), [](float v) { return std::isfinite(v); }),
                    "full pattern finite");
            require(maxDifference(raw, full) > 0.f, "full pattern changes flow");
            if (mode == 5) {
                bool analyticFixed = true;
                for (size_t i = 0; i < full.size(); i += 4) {
                    const double pixelAngle = std::atan2(full[i], -full[i+1]) * 180 / PI;
                    analyticFixed &= axialDistance(pixelAngle, style.fallbackAngle) < .04 &&
                        std::abs(full[i+2] - std::max(raw[i+2], style.minFieldMag)) < .00002f;
                }
                require(analyticFixed, "fixed-angle analytic direction and magnitude at every pixel");
                if (baselineDir) {
                    const auto old = read(oldField);
                    bool sameAxis = true;
                    for (size_t i = 0; i < full.size(); i += 4) {
                        const float same = std::max(std::abs(old[i] - full[i]), std::abs(old[i+1] - full[i+1]));
                        const float flip = std::max(std::abs(old[i] + full[i]), std::abs(old[i+1] + full[i+1]));
                        sameAxis &= std::min(same, flip) == 0.f && old[i+2] == full[i+2];
                    }
                    require(sameAxis, "fixed-angle full strength axis and magnitude unchanged");
                }
            }
            style.fieldMix = 0.f; uploadStyle(); dispatch(fixed, grad, newField);
            require(maxDifference(raw, read(newField)) == 0.f, "pattern strength zero exactly source");
            style.fieldMix = .0001f; uploadStyle(); dispatch(fixed, grad, newField);
            const auto epsilon = read(newField);
            bool nearMagnitude = true;
            for (size_t i = 2; i < raw.size(); i += 4)
                nearMagnitude &= std::abs(epsilon[i] - raw[i]) < .00002f;
            require(nearMagnitude && maxDifference(raw, epsilon) < .00002f &&
                    (flat || axialDistance(angle(epsilon), angle(raw)) < .04),
                    "pattern strength epsilon converges to source");
        }
        glNamedBufferSubData(etfMax, 0, sizeof(zero), &zero);
        etf.use(); glUniform1i(etf.uniform("uPass"), 0); dispatch(etf, grad, oldField);
        etf.use(); glUniform1i(etf.uniform("uPass"), 1); dispatch(etf, grad, oldField);
        checkFilteredSource(oldField, "ETF Follow image exact source");
        deepSuite(oldField,"ETF");
        if (!flat) {
            style = StyleParams{}; style.enabled = style.brushModel = 1.f;
            style.layerB[0][2] = float(PI / 6); uploadStyle();
            dispatch(fixed, grad, newField);
            const auto rotated = read(newField);
            require(axialDistance(angle(rotated), angle(raw) + 30) < .04, "explicit layer rotation");
            style.layerB[0][2] = 0; style.perturb = .3f; uploadStyle();
            dispatch(fixed, grad, newField);
            const auto wobble = read(newField);
            bool magUnchanged = true;
            for (size_t i = 2; i < raw.size(); i += 4) magUnchanged &= raw[i] == wobble[i];
            require(magUnchanged && maxDifference(raw, wobble) > 0.f, "explicit wobble preserves magnitude");
        }
        // Exercise the structure tensor path directly, without a blur so the
        // analytic ramp direction is exact. Tensor xy is unit length while z
        // remains the source confidence; Follow image must preserve both.
        features.use(); glUniform1i(features.uniform("uPass"), 1);
        dispatch(features, src, grad);
        glBindTextureUnit(1, grad);
        features.use(); glUniform1i(features.uniform("uPass"), 2);
        dispatch(features, src, oldField);
        const auto tensor = read(oldField);
        style = StyleParams{}; style.enabled = style.brushModel = 1.f;
        uploadStyle(); dispatch(fixed, oldField, newField);
        require(std::all_of(tensor.begin(), tensor.end(), [](float v) { return std::isfinite(v); }),
                "tensor finite");
        require(maxDifference(tensor, read(newField)) == 0.f, "tensor Follow image exact source");
        if (!flat) require(axialDistance(angle(tensor), c.expected) < .1, "tensor analytic tangent");
        checkFilteredSource(oldField, "tensor Follow image exact source and patterns");
        deepSuite(oldField,"tensor");
    }
    // Direct fields isolate source confidence from gradient direction and
    // add curved geometry that a linear image ramp cannot exercise.
    for(int fixture=0;fixture<6;++fixture){
        std::vector<float> field(W*H*4,0.f);
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){size_t i=(y*W+x)*4;
            double a=fixture<2?.31:(fixture==2?-1.13:std::atan2(y-31.5,x-31.5));
            float magnitude=fixture==0?.5f:(fixture==1?.0003f:(fixture==2?.08f:(fixture==3?.5f:.003f)));
            if(fixture==5&&x%7==0)magnitude=0;
            field[i]=float(std::sin(a)*magnitude);field[i+1]=float(-std::cos(a)*magnitude);field[i+2]=magnitude;
        }
        glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());
        char fixtureName[64];std::snprintf(fixtureName,sizeof(fixtureName),"direct fixture%d",fixture);deepSuite(grad,fixtureName);
        const auto raw=read(grad);
        // UI-valid extrema, including Voronoi samples outside image bounds.
        for(int mode=1;mode<6;++mode)for(int end=0;end<2;++end)for(float amount:{.25f,.5f,1.f}){
            style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=float(mode);style.fieldMix=amount;
            style.vortexX=end?1.f:0.f;style.vortexY=end?0.f:1.f;style.vortexRadius=end?1.f:.05f;style.vortexSpiral=end?1.5f:-1.5f;
            style.patchCell=end?160.f:4.f;style.patchJitter=end?1.5f:0.f;style.patchQuant=end?1.5f:0.f;
            style.curlScale=end?600.f:8.f;style.waveLen=end?1000.f:8.f;style.waveAmp=end?2.f:0.f;
            style.waveAngle=end?float(PI):float(-PI);style.fallbackAngle=end?180.f:-180.f;
            deepOne(grad,raw,end?"maximum controls":"minimum controls");
        }
    }
    // Curl and Waves defer completely to fully confident source directions.
    // Patches with no jitter/quantization preserve a constant confident field.
    {
        std::vector<float> field(W*H*4,0.f);
        for(size_t i=0;i<field.size();i+=4){field[i+1]=-.5f;field[i+2]=.5f;}
        glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());const auto raw=read(grad);
        for(int mode:{2,3,4}){
            style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=float(mode);style.patchJitter=style.patchQuant=0;
            uploadStyle();dispatch(fixed,grad,newField);
            require(axialDistance(angle(read(newField)),angle(raw))<.04,"confident constant-source direction invariant");
        }
        style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=4;style.waveAmp=0;style.waveAngle=.7f;
        // Flat source: zero wave amplitude must give the user-selected angle.
        std::fill(field.begin(),field.end(),0.f);glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());
        uploadStyle();dispatch(fixed,grad,newField);
        require(axialDistance(angle(read(newField)),.7*180/PI)<.04,"zero-amplitude waves selected angle");
        // A vortex eye has no radial direction: its deterministic orientation
        // should be the source/base orientation, not atan(0,0)'s driver value.
        for(size_t i=0;i<field.size();i+=4){field[i+1]=-.5f;field[i+2]=.5f;}
        glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());
        style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=1;style.vortexSpiral=0;
        style.vortexX=style.vortexY=(W/2+.5f)/W;uploadStyle();dispatch(fixed,grad,newField);
        std::printf("SINGULAR swirl eye angle %.6f deg, expected source0\n",angle(read(newField)));
        require(axialDistance(angle(read(newField)),0)<.04,"swirl exact-eye uses source orientation");
    }    // Equivalent unoriented axes must stay equivalent at half strength too.
    // +90 and -90 denote exactly the same stroke orientation.
    {
        std::vector<float> horizontal(W*H*4,0.f);
        for(size_t i=0;i<horizontal.size();i+=4) {horizontal[i+1]=-.5f;horizontal[i+2]=.5f;}
        glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,horizontal.data());
        style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=5;style.fieldMix=.5f;
        style.fallbackAngle=90;uploadStyle();dispatch(fixed,grad,newField);const auto plus=read(newField);
        style.fallbackAngle=-90;uploadStyle();dispatch(fixed,grad,newField);const auto minus=read(newField);
        std::printf("AXIAL cancellation: fixed +90 yields %.6f, fixed -90 yields %.6f, difference %.6f deg\n",
                    angle(plus),angle(minus),axialDistance(angle(plus),angle(minus)));
        require(axialDistance(angle(plus),angle(minus))<.05,"equivalent fixed axes at orthogonal half blend");
    }
    for(double sourceAngle:{15.,30.,45.,60.}){
        std::vector<float> field(W*H*4,0.f);double a=sourceAngle*PI/180;
        for(size_t i=0;i<field.size();i+=4){field[i]=float(std::sin(a)*.5);field[i+1]=float(-std::cos(a)*.5);field[i+2]=.5f;}
        glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());
        style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=5;style.fieldMix=.5f;
        style.fallbackAngle=float(sourceAngle+90);uploadStyle();dispatch(fixed,grad,newField);const auto plus=read(newField);
        style.fallbackAngle=float(sourceAngle-90);uploadStyle();dispatch(fixed,grad,newField);const auto minus=read(newField);
        double distance=axialDistance(angle(plus),angle(minus));
        std::printf("AXIAL source%.0f target%.0f/%.0f half: %.6f vs %.6f, distance%.6f\n",sourceAngle,sourceAngle+90,sourceAngle-90,angle(plus),angle(minus),distance);
        const auto quantized=read(grad);Vec condition=expectedAxis(W/2,H/2,quantized,style);
        double tolerance=.05+1e-6/std::max(std::hypot(condition.x,condition.y),1e-6)*180/PI;
        require(distance<tolerance,"equivalent near-perpendicular axes within conditioned float precision");
    }
    // Full precision inputs isolate an exact axial tie from RGBA16F source
    // quantization. The incoming direction is the deterministic tie contract.
    const GLuint exactField=glu::createTexture2D(W,H,GL_RGBA32F);
    for(double sourceAngle:{0.,15.,30.,45.,60.,90.}){
        std::vector<float> field(W*H*4,0.f);double a=sourceAngle*PI/180;
        for(size_t i=0;i<field.size();i+=4){field[i]=float(std::sin(a)*.5);field[i+1]=float(-std::cos(a)*.5);field[i+2]=.5f;}
        glTextureSubImage2D(exactField,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());
        for(float amount:{.499f,.5f,.501f}){
            style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=5;style.fieldMix=amount;
            style.fallbackAngle=float(sourceAngle+90);uploadStyle();dispatch(fixed,exactField,newField);const auto plus=read(newField);
            style.fallbackAngle=float(sourceAngle-90);uploadStyle();dispatch(fixed,exactField,newField);const auto minus=read(newField);
            require(axialDistance(angle(plus),angle(minus))<.1,"exact perpendicular equivalent fixed axes");
            if(amount==.5f){
                std::printf("EXACT TIE source%.0f output%.6f\n",sourceAngle,angle(plus));
                require(axialDistance(angle(plus),sourceAngle)<.04,"exact perpendicular half-blend keeps source axis");
            }
        }
        for(float target:{-180.f,-90.f,0.f,90.f,180.f}){
            style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=5;style.fallbackAngle=target;
            uploadStyle();dispatch(fixed,exactField,newField);
            require(axialDistance(angle(read(newField)),target)<.04,"fixed-angle full-strength cardinal axes");
        }
    }
    {
        std::vector<float> field(W*H*4,0.f);
        for(size_t i=0;i<field.size();i+=4){field[i]=.04f;field[i+1]=-.04f;field[i+2]=.04f;}
        glTextureSubImage2D(exactField,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());
        style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=2;
        style.fallbackAngle=135;style.patchJitter=style.patchQuant=0;uploadStyle();dispatch(fixed,exactField,newField);
        require(axialDistance(angle(read(newField)),45)<.04,"patch seed half-confidence axial tie keeps seed source axis");
    }    // Tail dispatches must not write outside IMG_SIZE, even though texture
    // storage and launched workgroups are deliberately larger than the image.
    for(const auto dims:{std::pair<int,int>{1,1},{3,3},{17,23},{63,61}}){
        params.imgW=float(dims.first);params.imgH=float(dims.second);
        params.texelW=1.f/dims.first;params.texelH=1.f/dims.second;
        glNamedBufferSubData(tuning,0,sizeof(params),&params);
        std::vector<float> field(W*H*4,0.f),sentinel(W*H*4,-7.f);
        for(size_t i=0;i<field.size();i+=4){field[i]=.03f;field[i+1]=-.04f;field[i+2]=.05f;}
        glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,field.data());const auto source=read(grad);
        for(int mode=0;mode<6;++mode)for(float amount:{0.f,.5f,1.f}){
            glTextureSubImage2D(newField,0,0,0,W,H,GL_RGBA,GL_FLOAT,sentinel.data());
            style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=float(mode);style.fieldMix=amount;
            uploadStyle();dispatch(fixed,grad,newField);const auto result=read(newField);bool safe=true;
            for(int y=0;y<H;++y)for(int x=0;x<W;++x)for(int k=0;k<4;++k){size_t i=(y*W+x)*4+k;
                if(x>=dims.first||y>=dims.second)safe&=result[i]==-7.f;
                else safe&=std::isfinite(result[i])&&((mode==0||amount==0)?result[i]==source[i]:true);
            }
            char label[160];std::snprintf(label,sizeof(label),"dispatch boundary %dx%d mode%d strength%.1f",dims.first,dims.second,mode,amount);require(safe,label);
        }
    }
    params.imgW=W;params.imgH=H;params.texelW=1.f/W;params.texelH=1.f/H;glNamedBufferSubData(tuning,0,sizeof(params),&params);
    {
        std::vector<float> flat(W*H*4,0.f);glTextureSubImage2D(grad,0,0,0,W,H,GL_RGBA,GL_FLOAT,flat.data());
        style=StyleParams{};style.enabled=style.brushModel=1;style.fieldMode=1;style.vortexX=style.vortexY=(W/2+.5f)/W;
        uploadStyle();dispatch(fixed,grad,newField);
        require(axialDistance(angle(read(newField)),style.fallbackAngle)<.04,"flat swirl exact-eye uses base fallback axis");
        style.curlScale=110.f;uploadStyle();
        glUseProgram(curlProbe);glBindImageTexture(0,newField,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA16F);
        glDispatchCompute(W / 16,H / 16,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
        const auto probe=read(newField);size_t exactZeroCount=0;bool zeroAngle=true;int firstX=-1,firstY=-1;
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){size_t i=(y*W+x)*4;if(probe[i+3]==1.f){++exactZeroCount;if(firstX<0){firstX=x%57;firstY=y%57;}zeroAngle&=probe[i+2]==0.f&&std::isfinite(probe[i+2]);}}
        if(firstX>=0)std::printf("CURL helper exact-zero candidates %zu, first p %.0f,%.0f, zero-angle %s\n",exactZeroCount,std::exp2(20.0+firstX*.25),std::exp2(20.0+firstY*.25),zeroAngle?"yes":"no");
        else std::printf("CURL helper exact-zero candidates 0, zero-angle no\n");
        require(exactZeroCount>0&&zeroAngle,"production curlAngle exact-zero finite-difference stagnation uses zero axis");
    }    std::printf("Deep matrix: %d dispatches, %zu per-pixel comparisons\n",deepDispatches,deepPixels);    require(glGetError() == GL_NO_ERROR, "no OpenGL errors");
    std::printf("Failures: %d\n", failures);
    glDeleteProgram(curlProbe);
    glfwDestroyWindow(window); glfwTerminate();
    return failures ? 1 : 0;
}
