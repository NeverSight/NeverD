package fixture;
public final class Provider<E> {
 public Object firstValue;
 public Leaf<E,Token> mappingValue;
 public Provider(Object firstValue,Leaf<E,Token> mappingValue) {this.firstValue=firstValue;this.mappingValue=mappingValue;}
 public final Object first() {
  Trace.hit(1);Object saved=firstValue;Trace.firstReceiver=this;
  firstValue=Trace.replacementFirst;Trace.hit(2);return saved;
 }
 public final Leaf<E,Token> mapping() {
  Trace.hit(3);Leaf<E,Token> saved=mappingValue;Trace.mappingReceiver=this;
  firstValue=Trace.replacementAgain;mappingValue=(Leaf<E,Token>)Trace.replacementMap;
  Trace.hit(4);return saved;
 }
}
