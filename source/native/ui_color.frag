#version 450 core
layout(location=0) in struct { vec4 Color; vec2 UV; } In;
layout(location=0) out vec4 fColor;
layout(set=0,binding=0) uniform sampler2D sTexture;
vec3 linearize(vec3 c) { return mix(pow(max((c+0.055)/1.055,vec3(0)),vec3(2.4)),c/12.92,lessThanEqual(c,vec3(0.04045))); }
vec3 pq(vec3 nits) { vec3 p=pow(max(nits/10000.0,vec3(0)),vec3(2610.0/16384.0)); return pow((3424.0/4096.0+(2413.0/128.0)*p)/(1.0+(2392.0/128.0)*p),vec3(2523.0/32.0)); }
void main() {
    fColor=In.Color*texture(sTexture,In.UV.st);
    vec3 c=linearize(fColor.rgb);
#if MFG_COLOR == 1
    fColor.rgb=c*(203.0/80.0);
#elif MFG_COLOR == 2
    vec3 rec2020=vec3(dot(c,vec3(0.627404,0.329282,0.0433136)),dot(c,vec3(0.069097,0.919540,0.0113612)),dot(c,vec3(0.0163916,0.0880132,0.895595)));
    fColor.rgb=pq(rec2020*203.0);
#elif MFG_COLOR == 3
    fColor.rgb=c;
#endif
}
