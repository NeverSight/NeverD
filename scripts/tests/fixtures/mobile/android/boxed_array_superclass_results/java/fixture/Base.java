package fixture;
public abstract class Base<T> { protected Base() {} public abstract T get(); public T parentGet() { return get(); } }
