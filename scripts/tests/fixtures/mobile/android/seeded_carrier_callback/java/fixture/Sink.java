package fixture;
public interface Sink<A,B> {void accept(A input,Target<B> target);}
