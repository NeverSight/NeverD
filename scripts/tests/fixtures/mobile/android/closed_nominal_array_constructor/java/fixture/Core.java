package fixture;
public class Core {
  public final java.util.List<String> names;
  public final String[] labels;
  public final Item[][] items;
  public final Context context;
  public Object value;
  static {Trace.hit(1);}
  public Core(java.util.List<String> names,String[] labels,Item[][] items,Context context) {
    Trace.calls++;Trace.hit(2);
    this.names=names;this.labels=labels;this.items=items;this.context=context;
    Trace.seen=this;Trace.action.run();Trace.hit(7);value=Trace.result;
  }
  public Core(Object ignored,String[] labels,Item[][] items,Context... context) {
    Trace.calls+=10;Trace.hit(2);this.names=null;this.labels=labels;this.items=items;this.context=null;
  }
  public Core(Item[][] items,Context context,String... labels) {this(null,labels,items,context);Trace.hit(8);}
  public Core(Item[][] items,String... labels) {this(items,null,labels);Trace.hit(9);}
  public Core(String... labels) {this((Item[][])null,labels);}
}
