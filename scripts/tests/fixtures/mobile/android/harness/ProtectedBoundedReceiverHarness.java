import api.Bound;
import fixture.*;
import other.Base;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedBoundedReceiverHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned protected bounded receiver"); }
    static final class Output implements Bound { public void touch() { throw new AssertionError("output must not be touched"); } }
    static class Input implements Bound {
        final Child receiver;final Bound replacement;final boolean replace;
        Input(Child receiver,Bound replacement,boolean replace) { this.receiver=receiver;this.replacement=replacement;this.replace=replace; }
        public void touch() {
            require(Trace.receiver==receiver&&Trace.input==this);
            Trace.touched=this;if(replace)Trace.output=replacement;Trace.hit(4);
        }
    }
    static final class DerivedInput extends Input { DerivedInput(Child receiver,Bound replacement,boolean replace) { super(receiver,replacement,replace); } }
    static Bound call(int route,Child receiver,Bound input) throws Throwable {
        if(route==0)return receiver.run(input);
        if(route==1)return ProtectedBoundedReceiverCaller.call(receiver,input);
        if(route==2) { try { return (Bound)Child.class.getMethod("run",Bound.class).invoke(receiver,input); } catch(InvocationTargetException e) { throw e.getCause(); } }
        return (Bound)MethodHandles.publicLookup().findVirtual(Child.class,"run",MethodType.methodType(Bound.class,Bound.class)).invokeExact(receiver,input);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Base.class.getDeclaredMethod("choose",Bound.class);
        require(Modifier.isAbstract(Base.class.getModifiers())&&Base.class.getTypeParameters().length==0);
        require(Modifier.isProtected(method.getModifiers())&&Modifier.isFinal(method.getModifiers())&&!Modifier.isStatic(method.getModifiers()));
        TypeVariable<?>[] formals=method.getTypeParameters();require(formals.length==1&&formals[0].getBounds().length==1&&formals[0].getBounds()[0]==Bound.class);
        require(method.getGenericReturnType().equals(formals[0])&&method.getGenericParameterTypes()[0].equals(formals[0])&&method.getExceptionTypes().length==0);
        int[] failures={0,0,1,4,2,3,5,6,0,0,0,0};
        for(int attempt=0;attempt<2;attempt++) {
            Child receiver=mode==10?null:new Child();
            Bound initial=mode==8?null:new Output(),replacement=mode==11?null:new Output();
            boolean replace=mode==9||mode==11;
            Bound input=mode==1?null:(attempt==0?new Input(receiver,replacement,replace):new DerivedInput(receiver,replacement,replace));
            Bound expected=replace?replacement:initial;
            Trace.events=0;Trace.failAt=failures[mode];Trace.failure=new RuntimeException("owned failure");
            Trace.receiver=Trace.input=Trace.touched=Trace.result=Trace.after=null;Trace.output=initial;
            Bound actual=null;Throwable caught=null;
            try { actual=call(route,receiver,input); } catch(Throwable e) { caught=e; }
            if(mode==1||mode==10)require(caught instanceof NullPointerException);
            else if(Trace.failAt!=0)require(caught==Trace.failure);
            else require(caught==null&&actual==expected&&actual!=input);
            long events=mode==10?0:mode==6?5:mode==1||mode==2?51:mode==3?514:mode==4?5142:mode==5?51423:514236;
            require(Trace.events==events);
            boolean entered=mode!=6&&mode!=10,touched=entered&&mode!=1&&mode!=2;
            boolean resultStored=touched&&mode!=3&&mode!=4,afterStored=resultStored&&mode!=5;
            require(Trace.receiver==(entered?receiver:null)&&Trace.input==(entered?input:null));
            require(Trace.touched==(touched?input:null)&&Trace.result==(resultStored?expected:null)&&Trace.after==(afterStored?expected:null));
            require(Trace.output==(touched&&replace?replacement:initial));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
