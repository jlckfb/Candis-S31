#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "player_utils.h"

int main(void)
{
    assert(player_media_file_supported("movie.avi"));
    assert(player_media_file_supported("MOVIE.AVI"));
    assert(!player_media_file_supported("clip.Mp4"));
    assert(!player_media_file_supported("movie.mjpeg"));
    assert(!player_media_file_supported("avi"));
    assert(!player_media_file_supported(NULL));

    assert(player_clamp_percent(-1) == 0);
    assert(player_clamp_percent(48) == 48);
    assert(player_clamp_percent(101) == 100);

    assert(player_percent_to_ms(25, 12000) == 3000);
    assert(player_percent_to_ms(50, UINT64_C(9000000000)) == UINT64_C(4500000000));
    assert(player_percent_to_ms(200, 12000) == 12000);
    assert(player_ms_to_percent(3000, 12000) == 25);
    assert(player_ms_to_percent(UINT64_C(4500000000), UINT64_C(9000000000)) == 50);
    assert(player_ms_to_percent(1, 0) == 0);
    assert(player_ms_to_percent(13000, 12000) == 100);
    return 0;
}
