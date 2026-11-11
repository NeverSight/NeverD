package fixture;
public abstract class Owner implements Api,Extra {
    @Override public int measure(float value) { return Api.super.measure(value); }
}
