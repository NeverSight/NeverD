package fixture;
public abstract class Base<T> { protected Base() {} protected abstract T get(); public T parentGet() { return get(); } }
