package fixture;
public abstract class Host<H> extends Base {
    public final Activity activity;
    public final Context context;
    public final Handler handler;
    public final int code;
    public final H payload;
    public Host(Activity activity,Context context,Handler handler,int code) {
        super();Trace.require(context,1);Trace.require(handler,2);
        this.activity=activity;this.context=context;this.handler=handler;this.code=code;this.payload=null;
        Trace.hit(3,this,activity,context,handler,code);
    }
    public Host(Context context,Handler handler,int code) {
        this(context instanceof Activity?(Activity)context:null,context,handler,code);
    }
    public Host(Activity activity) {
        this((Activity)Trace.require(activity,6),activity,new Handler(),0);
    }
}
