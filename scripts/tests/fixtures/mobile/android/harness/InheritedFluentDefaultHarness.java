import fixture.*;
import java.lang.reflect.*;
public final class InheritedFluentDefaultHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned inherited fluent default");}
    static void metadata(String accessor,String forward,boolean projected)throws Exception{
        TypeVariable<?>[] own=Child.class.getTypeParameters();require(own.length==1 && own[0].getGenericDeclaration()==Child.class && own[0].getBounds()[0]==Object.class);
        Method raw=Child.class.getDeclaredMethod(accessor,Child.class,Parent.class);require(raw.getReturnType()==Parent.class && raw.getGenericReturnType()==Parent.class && raw.getTypeParameters().length==0 && raw.isSynthetic()==!projected);
        require(raw.getGenericParameterTypes()[0]==Child.class && raw.getGenericParameterTypes()[1]==Parent.class);
        Method parent=Parent.class.getDeclaredMethod("combine",Parent.class);require(parent.isDefault() && parent.getTypeParameters().length==0 && parent.getGenericParameterTypes()[0]==Parent.class && parent.getGenericReturnType()==Parent.class);
        Method entry=Class.forName("fixture.Child$DefaultImpls").getDeclaredMethod(forward,Child.class,Parent.class);TypeVariable<?>[] vars=entry.getTypeParameters();require(vars.length==1 && vars[0].getGenericDeclaration().equals(entry) && vars[0].getBounds()[0]==Object.class);
        ParameterizedType self=(ParameterizedType)entry.getGenericParameterTypes()[0];require(self.getRawType()==Child.class && self.getActualTypeArguments()[0].equals(vars[0]) && entry.getGenericParameterTypes()[1]==Parent.class && entry.getGenericReturnType()==Parent.class);
    }
    public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),alias=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);metadata(args[3],args[4],Boolean.parseBoolean(args[5]));
        for(int attempt=0;attempt<2;attempt++){
            Trace.calls=Trace.virtuals=Trace.events=Trace.choice=Trace.fail=0;Trace.receiver=Trace.input=null;
            RuntimeException failure=new IllegalArgumentException("owned exact failure");Trace.failure=failure;
            InheritedFluentDefaultCaller.Receiver receiver=alias==0?new InheritedFluentDefaultCaller.Receiver():new InheritedFluentDefaultCaller.Derived();
            Parent other=new InheritedFluentDefaultCaller.Receiver(),expected=other;
            if(mode==1)other=expected=null;if(mode==2)other=expected=receiver;
            if(mode==3){Trace.choice=1;expected=receiver;}
            if(mode==4){Trace.choice=2;Trace.replacement=expected=new InheritedFluentDefaultCaller.Derived();}
            if(mode==5){Trace.choice=3;expected=null;}
            if(mode==6){Trace.choice=2;Trace.replacement=new Object();}
            Trace.fail=mode==7?1:mode==8?2:0;if(mode==9)receiver=null;
            Parent value=null;Throwable seen=null;try{value=InheritedFluentDefaultCaller.call(route,receiver,other,args[3],args[4]);}catch(Throwable error){seen=error;}
            if(mode<=5)require(seen==null && value==expected);
            else if(mode==6)require(seen!=null && seen.getClass()==ClassCastException.class);
            else if(mode==9)require(seen!=null && seen.getClass()==NullPointerException.class);
            else require(seen==failure);
            require(Trace.calls==(mode==9?0:1) && Trace.virtuals==0 && Trace.events==(mode==9?0:mode==7?1:12));
            require(Trace.receiver==receiver && Trace.input==(mode==7 || mode==9?null:other));
        }
        System.out.println("attempts=2,inherited-fluent-default=matched");
    }
}
