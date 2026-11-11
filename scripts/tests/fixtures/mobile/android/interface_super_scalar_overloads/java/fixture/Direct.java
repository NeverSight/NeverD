package fixture;
public final class Direct implements Api,Extra {
    @Override public int measure(float value) { return Api.super.measure(value); }
    @Override public float measure(int value) { return Api.super.measure(value); }
    @Override public double measure(long value) { return Api.super.measure(value); }
}
