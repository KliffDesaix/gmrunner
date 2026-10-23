// Desktop preview of the Switch game list: prints the folder chosen.
#include <cstdio>

extern "C" int gmr_switch_launcher(const char* root, char* out, int outSize);

int main(int argc, char** argv) {
    char buf[1024] = {};
    int r = gmr_switch_launcher(argc > 1 ? argv[1] : "games", buf, sizeof buf);
    printf("result %d: %s\n", r, buf);
    return 0;
}
