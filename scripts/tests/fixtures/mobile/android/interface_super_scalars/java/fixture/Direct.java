package fixture;
public final class Direct implements Api,Extra {
    @Override public int measure(float value) { return Api.super.measure(value); }
}
