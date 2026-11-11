package fixture;
public interface Api<P> {
    Object emit(P value, Sink<? super Token> tail);
}
