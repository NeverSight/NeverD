package fixture;
public interface Completion<E> { Object context(); void accept(E value); }
