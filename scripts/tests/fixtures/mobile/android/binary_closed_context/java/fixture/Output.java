package fixture;
public interface Output<P,R> extends Input<P,R> {R apply(P value)throws RuntimeException;}
