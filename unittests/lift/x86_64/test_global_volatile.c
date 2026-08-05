volatile int g_counter = 0;
int g_value = 42;

int increment_and_read(void) {
    g_counter++;
    return g_counter + g_value;
}

int write_and_sum(int x) {
    g_value = x;
    g_counter = x + 1;
    return g_value + g_counter;
}
