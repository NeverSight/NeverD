import api.*;
import impl.*;
import fixture.Trace;
import java.lang.reflect.*;
import java.lang.invoke.*;
public class ProtectedAbstractNamesHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned protected abstract names"); }
    static boolean call(int route,Root receiver,Press scope,long bits,String name) throws Throwable {
        if(route==0)return receiver.run(scope,bits);
        if(route==1)return ProtectedAbstractNamesCaller.call(receiver,scope,bits);
        if(route==2) {
            Method m=Root.class.getDeclaredMethod(name,Press.class,long.class);m.setAccessible(true);
            try { return (Boolean)m.invoke(receiver,scope,bits); }catch(InvocationTargetException e) { throw e.getCause(); }
        }
        if(route==3)return (boolean)MethodHandles.publicLookup().findVirtual(Root.class,"run",MethodType.methodType(boolean.class,Press.class,long.class)).invokeExact(receiver,scope,bits);
        if(route==4)return ProtectedAbstractNamesCaller.peer(receiver,scope,bits);
        return Factory.local(receiver,scope,bits);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);String name=args[3];
        Method root=Root.class.getDeclaredMethod(name,Press.class,long.class);
        require(Modifier.isProtected(root.getModifiers())&&Modifier.isAbstract(root.getModifiers())&&root.getTypeParameters().length==0&&root.getReturnType()==boolean.class);
        for(int attempt=0;attempt<2;attempt++) {
            Trace.constructed=0;Root created=Factory.create(kind);require(Trace.constructed==1);
            Method implementation=created.getClass().getDeclaredMethod(name,Press.class,long.class);
            require(Modifier.isProtected(implementation.getModifiers())&&!Modifier.isStatic(implementation.getModifiers())&&Modifier.isFinal(implementation.getModifiers())==(kind==0));
            require(Modifier.isFinal(created.getClass().getModifiers())==(kind==1));
            Root receiver=mode==3?null:created;
            long bits=mode==1?Long.MIN_VALUE:mode==9?Long.MAX_VALUE:0x8123456789abcdefL+attempt;
            boolean initial=mode==1;Trace.output=initial;
            Press scope=mode==2?null:new Press() { public void touch() { require(Trace.receiver==receiver&&Trace.scope==this&&Trace.bits==bits);Trace.touched=this;if(mode==7)Trace.output=!Trace.output;Trace.hit(5); } };
            Trace.receiver=Trace.scope=Trace.touched=null;Trace.bits=0;Trace.events=0;Trace.result=false;Trace.published=false;
            Trace.failAt=mode==4?2:mode==5?5:mode==6?3:mode==8?4:0;Trace.failure=new RuntimeException("owned failure");
            boolean result=false;Throwable caught=null;
            try { result=call(route,receiver,scope,bits,name); }catch(Throwable error) { caught=error; }
            boolean wrapped=route==0||route==1||route==3;
            boolean failure=mode==2||mode==3||mode==4||mode==5||mode==6||(mode==8&&wrapped);
            boolean output=mode==7?!initial:initial,expected=kind==0?output:!output;
            if(mode==2||mode==3)require(caught instanceof NullPointerException);else if(failure)require(caught==Trace.failure);else require(caught==null&&result==expected);
            long events=mode==3?0:mode==2||mode==4?2:mode==5?25:253;
            if(mode!=3&&wrapped)events=(mode==2||mode==4?12:mode==5?125:mode==6?1253:12534);
            require(Trace.events==events&&Trace.output==output&&Trace.constructed==1);
            require(Trace.receiver==(mode==3?null:receiver)&&Trace.scope==(mode==3?null:scope)&&Trace.bits==(mode==3?0:bits));
            require(Trace.touched==(mode==2||mode==3||mode==4?null:scope));
            boolean published=mode!=2&&mode!=3&&mode!=4&&mode!=5&&mode!=6;
            require(Trace.published==published&&Trace.result==(published?expected:false));
            System.out.println("route="+route+",kind="+kind+",mode="+mode+",attempt="+attempt+",events="+events);
        }
    }
}
