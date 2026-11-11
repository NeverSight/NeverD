package fixture;
public abstract class Factory<T> {
    protected Factory() {}
    protected abstract <T extends java.lang.annotation.Annotation> T create(String name,Class<T> token,Context context);
}
