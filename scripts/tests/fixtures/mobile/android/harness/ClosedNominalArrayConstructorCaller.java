import fixture.*;
public final class ClosedNominalArrayConstructorCaller {
  public static Core full(Item[][] items,Context context,String[] labels){return new Core(items,context,labels);}
  public static Core shortCall(Item[][] items,String[] labels){return new Core(items,labels);}
}
