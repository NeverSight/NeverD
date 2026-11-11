package fixture;
public final class Core {
 public static Object call(Source<Object> source,boolean noReceiver) {
  Cell.Factory factory=noReceiver?null:Cell.FACTORY;
  return factory.convert(source);
 }
 public static Object box(Object value){return new Cell<Object>(value);}
 private static final class Cell<E> {
  static final Factory FACTORY=new Factory();
  public final E value;
  Cell(E input){Trace.hit(2,this,input,null);value=input;Trace.hit(3,this,input,null);}
  public static final class Factory {
   @SuppressWarnings("unchecked")
   public final <U> Cell<U> convert(Source<U> source) {
    Trace.hit(0,this,source,null);
    Cell<U> allocated=new Cell<U>(source.read());
    Trace.hit(4,this,source,allocated);
    return Trace.override?(Cell<U>)Trace.replacement:allocated;
   }
  }
 }
}
