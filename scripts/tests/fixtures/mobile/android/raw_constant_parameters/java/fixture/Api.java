package fixture;
public interface Api<T> {
    Object run(Box<? super T> input, Context<? super Token> context);
}
