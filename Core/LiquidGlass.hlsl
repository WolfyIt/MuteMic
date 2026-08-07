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
    // abs() en la base: pc siempre es positivo, pero fxc no puede saberlo y
    // pow() con base negativa devuelve NaN (píxel negro). X3571.
    return 1.0 - pb * pow(abs(pc * M_E), -pd * x - pa);
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

    // UN SOLO punto de salida: los `return` tempranos hacían que fxc
    // avisara de la variable de salida del macro D2D_PS_ENTRY sin
    // inicializar en todos los caminos (X4000).
    float4 result;

    // Dos casos comparten exactamente la misma operación —fetch 1:1 en la
    // posición real, sin refractar—: fuera de la lente (esquinas más allá
    // del radio) e interior plano (más allá de la banda, donde la curva ya
    // vale 1). Muestrear en `pos` en vez de en una posición calculada
    // garantiza copia texel a texel: si se deja al filtro bilineal, basta
    // un error de fracción de píxel para que el texto de atrás pierda
    // nitidez. De paso ahorra toda la ALU de refracción y glow en la mayor
    // parte de la ventana.
    if (d > 0.0 || -d >= bandPx)
    {
        result = D2DSampleInputAtPosition(0, pos);
        // El ruido es parte del look "frosted" y solo aplica DENTRO de la
        // lente. (El blur, cuando lo hay, se aplica antes en el grafo de
        // efectos, así que no se pierde por esta vía.)
        if (d <= 0.0)
            result.rgb += (rnd(pos * 1e-3) - 0.5) * noiseAmt;
    }
    else
    {
        // Banda de refracción: profundidad normalizada 0..1 desde el borde.
        float t = saturate(-d / bandPx);

        // Refracción del repo: escalar la posición hacia el centro según
        // f(t). max() protege de una base negativa → NaN (X3571).
        float k = pow(max(fcurve(t), 1e-4), fPower);
        float2 spos = halfSize + pc2 * k;

        result = D2DSampleInputAtPosition(0, spos);
        result.rgb += (rnd(pos * 1e-3) - 0.5) * noiseAmt;

        // Glow direccional del repo: sin(atan2(y,x) - 0.5), atenuado por
        // borde.
        float ang = atan2(pc2.y, pc2.x);
        float mul = sin(ang - 0.5) * glowWeight * smoothstep(glowE0, glowE1, t)
                    + 1.0 + glowBias;
        result.rgb *= mul;
    }

    result.a = 1.0;
    return result;
}
