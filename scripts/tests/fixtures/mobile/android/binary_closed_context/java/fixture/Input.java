package fixture;
public interface Input<P,R> extends java.io.Serializable {R apply(P value)throws Exception;}
