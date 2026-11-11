package fixture; public final class Core {
 static {Trace.fire(0,null);}
 public final Object first; public final Second second; public final Context context;
 private Core(Object first,Second second,Context context){Trace.core=this;Trace.fire(7,this);this.first=first;this.second=second;this.context=context;Trace.fire(8,this);}
 public Core(long left,int right,Context context,Object seed){this(new First(left,context==null?null:context.left(seed)),new Second(right,context==null?null:context.right(seed)),context);}
 public Core(Object seed,Context context,long right){this(new Core((Object)null,(Second)null,context),new Second((int)right,context==null?null:context.right(seed)),context);}
}
