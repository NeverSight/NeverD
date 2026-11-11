package fixture;
public interface Completion<O> { Object context(); void accept(O value); }
