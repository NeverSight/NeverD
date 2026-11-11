import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ProjectedCaptureSuperHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned projected capture super"); }
    static final class Completion implements Sink<Object> {
        public void accept(Object value) { Trace.hit(6);Trace.callback=value;require(((Base)value).read()==Trace.seen); }
    }
    static Base make(int route,Owner<UpperBound> owner,UpperBound token,long wide,Sink<Object> completion,Base previous) throws Throwable {
        try {
            if(route==0)return owner.make(token,wide,completion);
            if(route==1)return ProjectedCaptureSuperCaller.make(owner,token,wide,completion);
            if(route==2)return (Base)Owner.class.getMethod("make",UpperBound.class,long.class,Sink.class).invoke(owner,token,wide,completion);
            if(route==3)return (Base)MethodHandles.publicLookup().findVirtual(Owner.class,"make",
                MethodType.methodType(Base.class,UpperBound.class,long.class,Sink.class)).invokeExact(owner,token,wide,completion);
            return previous.spawn(completion);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int[] failures={0,0,1,2,3,4,5,6,0};long[] traces={1426435,1426435,1,142,142643,14,1426435,1426,142};
        Owner<UpperBound> owner=new Owner<UpperBound>();Completion callback=new Completion();
        RuntimeException original=new IllegalStateException("owned projected capture failure");
        Base previous=null;
        UpperBound token=mode==1?null:new UpperBound();long wide=0x8123456789abcdefL;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.failAt=0;
            if(route==4)previous=owner.make(token,wide,callback);
            Sink<Object> incoming=mode==8?null:callback;
            Trace.events=0;Trace.partial=Trace.seen=Trace.callback=null;Trace.failure=original;Trace.failAt=failures[mode];
            Base result=null;boolean failing=Trace.failAt!=0||mode==8;
            try {result=make(route,owner,token,wide,incoming,previous);require(!failing);}
            catch(Throwable failure) {require(failing&&(mode==8?failure instanceof NullPointerException:failure==original));}
            require(Trace.events==traces[mode] && Trace.partial instanceof Base);
            Base partial=(Base)Trace.partial;
            require(partial.getClass().getField("token").get(partial)==token);
            require(partial.getClass().getField("wide").getLong(partial)==wide);
            require(partial.arity==(mode==2?0:1));
            require(partial.completion==(mode==2?null:incoming));
            require(Trace.seen==(mode==2||mode==5?null:token));
            require(Trace.callback==(mode==2||mode==3||mode==5||mode==7||mode==8?null:partial));
            if(!failing)require(result==partial&&result!=previous);
            Trace.failAt=0;
            require(owner.read(partial)==token);
            previous=result;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+traces[mode]);
        }
    }
}
