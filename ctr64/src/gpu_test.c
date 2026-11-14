/* M0 GPU regression test, kept as a boot option (hold SELECT).
 *
 * Pass 1 renders a test pattern into an N64-sized 320x240 region of a 512x256 render target.
 * Pass 2 samples that target directly as a texture onto the screen. Expected result:
 *   - corner markers: red top-left, green top-right, blue bottom-left, white bottom-right
 *   - a 1-pixel magenta border exactly on the edge of the 320x240 image
 *   - 8 color bars, two gradients, a spinning triangle and a bouncing square */

#include "gpu_test.h"

#include <math.h>
#include <stdio.h>

#include "video.h"

#define N64_W 320
#define N64_H 240

static void build_pattern(VideoBatch *b, u32 frame)
{
    static const u32 bars[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};

    for (int i = 0; i < 8; i++)
        video_batch_quad(b, i * 40.0f, 0, (i + 1) * 40.0f, 120, bars[i]);
    video_batch_quad4(b, 0, 120, N64_W, 170, 0x000000, 0xFFFFFF, 0x000000, 0xFFFFFF, 0, 0, 0, 0);
    video_batch_quad4(b, 0, 170, 160, 240, 0xFF0000, 0x00FF00, 0xFF0000, 0x00FF00, 0, 0, 0, 0);
    video_batch_quad4(b, 160, 170, N64_W, 240, 0x00FF00, 0x0000FF, 0x00FF00, 0x0000FF, 0, 0, 0, 0);

    int t = frame % 576;
    float sx = (float)(t < 288 ? t : 576 - t);
    video_batch_quad(b, sx, 196, sx + 32, 228, 0x202020);

    float ang = frame * 0.03f;
    const u32 tri_col[3] = {0xFFFF00, 0xFFFFFF, 0x00FFFF};
    for (int i = 0; i < 3; i++) {
        float a = ang + i * 2.0943951f;
        video_batch_vertex(b, 160 + 50 * cosf(a), 110 + 50 * sinf(a), tri_col[i], 0, 0);
    }

    video_batch_quad(b, 0, 0, 16, 16, 0xFF0000);
    video_batch_quad(b, N64_W - 16, 0, N64_W, 16, 0x00FF00);
    video_batch_quad(b, 0, N64_H - 16, 16, N64_H, 0x0000FF);
    video_batch_quad(b, N64_W - 16, N64_H - 16, N64_W, N64_H, 0xFFFFFF);
    video_batch_quad(b, 0, 0, N64_W, 1, 0xFF00FF);
    video_batch_quad(b, 0, N64_H - 1, N64_W, N64_H, 0xFF00FF);
    video_batch_quad(b, 0, 0, 1, N64_H, 0xFF00FF);
    video_batch_quad(b, N64_W - 1, 0, N64_W, N64_H, 0xFF00FF);
}

void gpu_test_run(void)
{
    PicaTarget rt;
    float proj[4][4];
    PicaTev tev;
    u32 frame = 0;

    if (!pica_target_create(&rt, 512, 256, GPU_RB_RGBA8, true, VRAM_ALLOC_A)) {
        printf("GPU test: render target allocation failed\n");
        return;
    }
    pica_mtx_ortho(proj, 0, N64_W, N64_H, 0, 0.0f, 1.0f);
    printf("GPU test pattern - START: quit\n\n");

    while (aptMainLoop()) {
        VideoBatch b;

        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;

        pica_frame_begin();
        pica_clear(&rt, pica_rgba8(0x10, 0x18, 0x30, 0xFF), true, 0xFFFFFF);
        pica_bind_target(&rt);
        pica_viewport(0, 0, N64_W, N64_H);
        video_use_program(proj);
        pica_texture(0, NULL, 0, 0, 0, 0);
        pica_tev_default(&tev);
        tev.src_rgb = tev.src_alpha = GPU_TEVSOURCES(GPU_PRIMARY_COLOR, 0, 0);
        pica_tev(0, &tev);
        if (video_batch_begin(&b, 256)) {
            build_pattern(&b, frame);
            video_batch_draw(&b);
        }

        video_draw_to_screen(rt.color, rt.width, rt.height, GPU_RGBA8, N64_W, N64_H, false, false);
        video_present();
        frame++;

        if ((frame % 30) == 0) {
            const PicaStats *s = pica_stats();
            printf("\x1b[3;0Hframe %-8lu\n", s->frames);
            printf("frame   %6.2f ms (%5.1f fps)   \n", s->frame_ms,
                   s->frame_ms > 0 ? 1000.0f / s->frame_ms : 0);
            printf("cpu     %6.2f ms   \n", s->cpu_ms);
            printf("gpuwait %6.2f ms   \n", s->fence_wait_ms);
            printf("cmd     %6lu bytes, %lu GX entries   \n", s->cmd_bytes, s->queue_entries);
            gfxFlushBuffers();
        }
    }
    pica_wait_idle();
    pica_target_destroy(&rt);
}
