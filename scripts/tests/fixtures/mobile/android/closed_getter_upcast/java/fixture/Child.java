package fixture;
public final class Child<E> extends Parent<E> {
 static { Trace.hit(9); }
 public final Provider<E> receiver;
 public Child(Provider<E> receiver) {
  Object first=receiver.first();Object erased=receiver.mapping();
  Mapping<E,Token> mapping=(Mapping<E,Token>)erased;
  super(first,mapping);this.receiver=receiver;Trace.hit(7);
 }
}
