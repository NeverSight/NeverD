package fixture;
public class Parent<E> {
 static { Trace.hit(8); }
 public final Object first;
 public final Mapping<E,Token> mapping;
 public Parent(Object first,Mapping<E,Token> mapping) {
  Trace.hit(5);this.first=first;this.mapping=mapping;Trace.seen=this;Trace.hit(6);
 }
}
