package fixture;
public interface Task<T> { T get() throws Retry, Failure; }
