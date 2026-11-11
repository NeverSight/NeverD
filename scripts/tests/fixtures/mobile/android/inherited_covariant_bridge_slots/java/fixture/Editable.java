package fixture;
public interface Editable extends Root {
    @Override Editable update(int code, Object[] values, long wide, double real);
    int scalar();
}
