package fixture;
public interface Source<S> extends Api<S>, Box<S> {
    void touch();
    @Override
    default Box<S> getBox() { touch(); return this; }
}
