package fixture;
public class Wrapper implements Api {
    public Class<?> witness;
    public Payload getStyle() { return Api.super.getStyle(); }
}
