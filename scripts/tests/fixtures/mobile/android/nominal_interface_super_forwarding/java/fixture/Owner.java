package fixture;
public abstract class Owner implements Api, Side {
    @Override public Output convert(Input value) { return Api.super.convert(value); }
}
