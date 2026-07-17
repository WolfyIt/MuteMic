// LiquidGlass.hlsl — refracción del repo OverShifted/LiquidGlass adaptada a
// la forma de la VENTANA: la lente es un rectángulo redondeado en píxeles
// (no el superellipse del demo, que en una ventana alta se veía como un
// círculo flotando). La refracción vive en una banda junto a los bordes
// (bandPx); el interior queda plano y nítido.
//
// Se compila en build con fxc (FxCompile en el vcxproj) y se carga en
// runtime con Win2D PixelShaderEffect. Input 0 = escritorio detrás de la
// ventana (pre-blurreado con Gaussian si frost).

#define D2D_INPUT_COUNT 1
#define D2D_INPUT0_COMPLEX
#define D2D_REQUIRES_SCENE_POSITION
#include "d2d1effecthelpers.hlsli"

cbuffer constants : register(b0)
{
    float2 winSize;      // tamaño de la ventana en px
    float cornerRad;     // radio de esquina de la lente (px)
    float bandPx;        // ancho de la banda de refracción (px)
    float fPower;        // exponente de f(x) ("f(x) Power" del repo)
    float pa;            // a
    float pb;            // b
    float pc;            // c
    float pd;            // d
    float noiseAmt;      // 0 = clean
    float glowWeight;    // 0 = sin glow
    float glowBias;
    float glowE0;
    float glowE1;
};

static const float M_E = 2.718281828459045;

// SDF de caja redondeada (px). Negativo adentro.
float sdRoundBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Curva de refracción del repo: f(x) = 1 - b * (c*e)^(-d*x - a)
// x en 0..1 (0 = borde, 1 = interior) → f≈<1 en el borde (dobla hacia el
// centro), f→1 en el interior (sin distorsión).
float fcurve(float x)
{
    return 1.0 - pb * pow(pc * M_E, -pd * x - pa);
}

float rnd(float2 co)
{
    return frac(sin(dot(co, float2(12.9898, 78.233))) * 43758.5453);
}

D2D_PS_ENTRY(main)
{
    float2 pos = D2DGetScenePosition().xy;   // px dentro de la ventana
    float2 halfSize = winSize * 0.5;
    float2 pc2 = pos - halfSize;             // centrado, en px

    float d = sdRoundBox(pc2, halfSize, cornerRad);
    if (d > 0.0)
    {
        // Fuera de la lente (esquinas más allá del radio): sin refractar.
        float4 c0 = D2DSampleInputAtPosition(0, pos);
        c0.a = 1.0;
        return c0;
    }

    // Profundidad normalizada dentro de la banda de refracción.
    float t = saturate(-d / bandPx);

    // Refracción del repo: escalar la posición hacia el centro según f(t).
    float k = pow(fcurve(t), fPower);
    float2 spos = halfSize + pc2 * k;

    float4 color = D2DSampleInputAtPosition(0, spos);
    color.rgb += (rnd(pos * 1e-3) - 0.5) * noiseAmt;

    // Glow direccional del repo: sin(atan2(y,x) - 0.5), atenuado por borde.
    float ang = atan2(pc2.y, pc2.x);
    float mul = sin(ang - 0.5) * glowWeight * smoothstep(glowE0, glowE1, t)
                + 1.0 + glowBias;
    color.rgb *= mul;
    color.a = 1.0;
    return color;
}
