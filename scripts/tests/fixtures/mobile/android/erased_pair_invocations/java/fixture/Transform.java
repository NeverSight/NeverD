package fixture;
public interface Transform<I, O> {
    I input();
    O decode(I value);
}
