package fixture;
public final class Concrete extends Host<String> {
    public Concrete(Activity activity,Context context,Handler handler,int code) {super(activity,context,handler,code);}
    public Concrete(Context context,Handler handler,int code) {super(context,handler,code);}
    public Concrete(Activity activity) {super(activity);}
}
