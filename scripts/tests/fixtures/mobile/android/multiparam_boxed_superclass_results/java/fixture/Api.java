package fixture;
public interface Api<T> { default Object at(Core receiver) { return receiver.get(); } }
