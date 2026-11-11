package fixture;
public interface Api extends Root {
    @Override Root update(int code, Object[] values, long wide, double real);
}
