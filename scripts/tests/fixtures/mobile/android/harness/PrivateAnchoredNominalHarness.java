import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class PrivateAnchoredNominalHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned private anchored result"); }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method target=Provider.class.getDeclaredMethod("dispatch",Reply.class,Cell.class,Callback.class,boolean.class,float.class,long.class);
        require(Modifier.isPrivate(target.getModifiers())&&!Modifier.isStatic(target.getModifiers())&&target.getReturnType()==Reply.class);
        TypeVariable<?> formal=target.getTypeParameters()[0];require(formal.getBounds().length==1&&formal.getBounds()[0]==Object.class);
        require(((ParameterizedType)target.getGenericParameterTypes()[1]).getActualTypeArguments()[0]==formal);
        Type[] cb=((ParameterizedType)target.getGenericParameterTypes()[2]).getActualTypeArguments();
        require(((WildcardType)cb[0]).getLowerBounds()[0]==formal&&cb[1]==Boolean.class);
        Provider owner=mode==10?null:new Provider();Reply incoming=new ReplyImpl(7);
        Object sentinel=new Object(),item=mode==11?null:new Object();
        Cell cell=mode==6?null:mode==13?new Cell<Object>(sentinel) {
            @Override public Object read() { Trace.reads++;Trace.hit(2);return Trace.nextInput; }
        }:new Cell<Object>(item);
        Trace.failure=new RuntimeException("owned private scope failure");
        Callback callback=mode==7?null:(Callback<Object,Boolean>)(input)->{
            Trace.callbacks++;Trace.hit(4);require(input==Trace.nextInput);
            if(mode==9)Trace.reply=null;else if(mode==12)Trace.reply=incoming;
            return mode==8?null:mode==11?Boolean.FALSE:Boolean.TRUE;
        };
        long[] wide={0L,-1L,Long.MIN_VALUE,Long.MAX_VALUE,0x0123456789abcdefL,0x7ff0000000000001L};
        for(int attempt=0;attempt<2;attempt++) {
            Object expectedInput=attempt==0?item:mode==11?null:new Object();Trace.nextInput=expectedInput;
            if(cell!=null&&mode!=13)cell.value=expectedInput;
            Reply offered=new ReplyImpl(attempt+20);Trace.reply=offered;
            Trace.events=0;Trace.receiver=null;Trace.input=sentinel;Trace.booleanResult=null;Trace.checked=false;
            Trace.flag=false;Trace.scale=0.0f;Trace.bits=0;
            Trace.failAt=mode>=1&&mode<=5?mode:0;
            long bits=wide[(mode+attempt)%wide.length];int raw=(mode==8?0x7fc01234:attempt==0?0x80000000:0x3f800000);
            float scale=Float.intBitsToFloat(raw);boolean flag=attempt==0;
            int beforeCalls=owner==null?0:owner.calls,beforeReads=Trace.reads,beforeCallbacks=Trace.callbacks;
            Reply result=null;Throwable failure=null;
            try {
                if(route==0)result=Core.call(owner,incoming,cell,callback,flag,scale,bits);
                else if(route==1)result=PrivateAnchoredNominalCaller.call(owner,incoming,cell,callback,flag,scale,bits);
                else if(route==2)result=(Reply)Core.class.getMethod("call",Provider.class,Reply.class,Cell.class,Callback.class,boolean.class,float.class,long.class).invoke(null,owner,incoming,cell,callback,flag,scale,bits);
                else result=(Reply)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Reply.class,Provider.class,Reply.class,Cell.class,Callback.class,boolean.class,float.class,long.class)).invoke(owner,incoming,cell,callback,flag,scale,bits);
            } catch(InvocationTargetException error) { failure=error.getCause(); }
              catch(Throwable error) { failure=error; }
            String steps=mode==10?"6":mode==1?"61":mode==6?"61":mode==2?"612":mode==7?"612":mode==4?"6124":mode==3||mode==8?"61243":"612435";
            require(Trace.events==Long.parseLong(steps));
            boolean entered=steps.indexOf('1')>=0&&mode!=1,read=steps.indexOf('2')>=0,called=steps.indexOf('4')>=0;
            require(Trace.receiver==(entered?owner:null));
            require((owner==null?0:owner.calls)==beforeCalls+(entered?1:0)&&Trace.reads==beforeReads+(read?1:0)&&Trace.callbacks==beforeCallbacks+(called?1:0));
            require(Trace.input==(read&&mode!=2?expectedInput:sentinel));
            if(entered)require(Trace.flag==flag&&Float.floatToRawIntBits(Trace.scale)==raw&&Trace.bits==bits);
            Boolean returned=mode==8?null:mode==11?Boolean.FALSE:Boolean.TRUE;
            require(Trace.booleanResult==(steps.indexOf('3')>=0?returned:null));
            require(Trace.checked==(steps.indexOf('5')>=0&&returned==Boolean.TRUE));
            if(mode>=1&&mode<=5)require(failure==Trace.failure);
            else if(mode==6||mode==7||mode==8||mode==10)require(failure instanceof NullPointerException);
            else require(failure==null&&result==(mode==9?null:mode==12?incoming:offered));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
