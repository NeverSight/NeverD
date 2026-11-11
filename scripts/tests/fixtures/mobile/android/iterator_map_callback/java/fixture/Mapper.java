package fixture;
public interface Mapper<P,R> extends java.io.Serializable {R apply(P value) throws Exception;}
