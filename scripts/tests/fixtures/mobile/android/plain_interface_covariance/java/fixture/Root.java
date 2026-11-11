package fixture;
public interface Root {
    Root update(int code, Object[] values, long wide, double real);
    Root update(Object value);
    int scalar();
}
