#include <cstdio>

static void (*volatile thrower)(int, int, int, int, int, int, int, int, int, int);

[[gnu::noinline]] void ThrowClangStackArguments(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
    if (a + b + c + d + e + f + g + h + i + j == 0) return;
    throw 42;
}

[[gnu::noinline]] bool CatchClangStackArguments() {
    try {
        thrower(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    } catch (int value) {
        return value == 42;
    }
    return false;
}

int main() {
    thrower = ThrowClangStackArguments;
    if (!CatchClangStackArguments()) {
        std::puts("exception stack pointer clang tests failed");
        return 1;
    }
    std::puts("exception stack pointer clang tests passed");
    return 0;
}
