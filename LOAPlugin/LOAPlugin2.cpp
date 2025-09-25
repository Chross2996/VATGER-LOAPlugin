// NonDiscreteSquawk.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "LOAPlugin.h"

LOAPlugin* pMyPlugIn = NULL;

void __declspec (dllexport)
EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	// allocate
	*ppPlugInInstance = pMyPlugIn =
		new LOAPlugin;
}

void __declspec (dllexport)
EuroScopePlugInExit(void)
{
	delete pMyPlugIn;
}